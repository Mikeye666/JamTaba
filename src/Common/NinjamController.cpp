#include "NinjamController.h"
#include "MainController.h"

#include "ninjam/Service.h"
#include "ninjam/User.h"
#include "ninjam/UserChannel.h"
#include "ninjam/Server.h"
#include "audio/core/AudioNode.h"
#include "audio/core/SamplesBuffer.h"
#include "audio/core/AudioDriver.h"
#include "audio/core/SamplesBuffer.h"
#include "file/FileReaderFactory.h"
#include "file/FileReader.h"
#include "audio/NinjamTrackNode.h"
#include "audio/MetronomeTrackNode.h"
#include "audio/Resampler.h"
#include "audio/SamplesBufferRecorder.h"
#include "audio/vorbis/VorbisEncoder.h"
#include "gui/NinjamRoomWindow.h"
#include "log/Logging.h"
#include "MetronomeUtils.h"
#include "persistence/Settings.h"
#include "Utils.h"

#include "chat/NinjamVotingMessageParser.h"

#include <QMutexLocker>
#include <QDebug>
#include <QThread>
#include <QFileInfo>
#include <QWaitCondition>

#include <cmath>
#include <cassert>
#include <vector>

using namespace controller;
using namespace gui;

//+++++++++++++  ENCODING THREAD  +++++++++++++

class NinjamController::EncodingThread : public QThread // TODO: use better thread approach, avoid inheritance form QThread
{

public:
    
    explicit EncodingThread(NinjamController* controller)
        : stopRequested(false), 
          controller(controller)
    {
        qCDebug(jtNinjamCore) << "Starting Encoding Thread";
        start();
    }

    ~EncodingThread()
    {
        stop();
    }

    void addSamplesToEncode(const audio::SamplesBuffer& samplesToEncode, quint8 channelIndex, bool isFirstPart, bool isLastPart)
    {
        //qCDebug(jtNinjamCore) << "Adding samples to encode";
        QMutexLocker locker(&mutex);
        chunksToEncode.push_back(EncodingChunk(samplesToEncode, channelIndex, isFirstPart, isLastPart));
        // this method is called by Qt main thread (the producer thread).
        hasAvailableChunksToEncode.wakeAll();//wakeup the encoding thread (consumer thread)

    }

    void stop()
    {
        if (!stopRequested) {
            //QMutexLocker locker(&mutex);
            stopRequested = true;
            hasAvailableChunksToEncode.wakeAll();
            qCDebug(jtNinjamCore) << "Stopping Encoding Thread";
        }
    }

    
protected:
    
    void run()
    {
        while (!stopRequested) {
            mutex.lock();
            if (chunksToEncode.empty()) {
                hasAvailableChunksToEncode.wait(&mutex);
            }
            
            if (stopRequested) {
                mutex.unlock();
                break;
            }
            

            EncodingChunk chunk = !chunksToEncode.empty() ? chunksToEncode.front() : EncodingChunk(audio::SamplesBuffer::ZERO_BUFFER, 0, false, false);

            chunksToEncode.erase(chunksToEncode.begin()); // remove first element

            mutex.unlock();
            
            if (!chunk.buffer.isEmpty()) {
                QByteArray encodedBytes(controller->encode(chunk.buffer, chunk.channelIndex));
                if (chunk.lastPart) {
                    encodedBytes.append(controller->encodeLastPartOfInterval(chunk.channelIndex));
                }

                if (!encodedBytes.isEmpty()) {
                    emit controller->encodedAudioAvailableToSend(encodedBytes, chunk.channelIndex, chunk.firstPart, chunk.lastPart);
                }

            }

        }
        qCDebug(jtNinjamCore) << "Encoding thread stopped!";
    }


private:
    
    class EncodingChunk 
    {
    public:
        EncodingChunk(const audio::SamplesBuffer& buffer, quint8 channelIndex, bool firstPart, bool lastPart) :
            buffer(buffer),
            channelIndex(channelIndex),
            firstPart(firstPart),
            lastPart(lastPart )
        {

        }

        audio::SamplesBuffer buffer;
        quint8 channelIndex;
        bool firstPart;
        bool lastPart;
    };

    std::vector<EncodingChunk> chunksToEncode;
    QMutex mutex;
    volatile bool stopRequested;
    NinjamController* controller;
    QWaitCondition hasAvailableChunksToEncode;
};

//+++++++++++++++++ Nested classes to handle schedulable events ++++++++++++++++

class NinjamController::SchedulableEvent // an event scheduled to be processed in next interval
{
public:
    explicit SchedulableEvent(NinjamController* controller) :
        controller(controller)
    {
        //
    }
    
    virtual void process() = 0;
    
    virtual ~SchedulableEvent()
    {
    
    }

protected:
    NinjamController* controller;
};

//++++++++++++++++++++++++++++++++++++++++++++

class NinjamController::BpiChangeEvent : public SchedulableEvent
{
public:
    BpiChangeEvent(NinjamController* controller, quint16 newBpi) :
        SchedulableEvent(controller),
        newBpi(newBpi)
    {
    }

    void process()
    {
        controller->currentBpi = newBpi;
        controller->samplesInInterval = controller->computeTotalSamplesInInterval();
        emit controller->currentBpiChanged(controller->currentBpi);
    }

private:
    quint16 newBpi;

};

//+++++++++++++++++++++++++++++++++++++++++

class NinjamController::BpmChangeEvent : public SchedulableEvent
{
public:
    BpmChangeEvent(NinjamController* controller, quint16 newBpm) :
        SchedulableEvent(controller),
        newBpm(newBpm)
    {
    
    }
    
    void process()
    {
        controller->setBpm(newBpm);
    }

private:
    quint16 newBpm;

};

//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

class NinjamController::InputChannelChangedEvent : public SchedulableEvent
{
    public:
        InputChannelChangedEvent(NinjamController* controller, int channelIndex) :
            SchedulableEvent(controller),
            channelIndex(channelIndex)
        {
        }
        
        void process()
        {
            controller->recreateEncoderForChannel(channelIndex);
        }
    
    private:
        int channelIndex;
};

//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

QList<QString> NinjamController::chatBlockedUsers; // initializing the static member

NinjamController::NinjamController(controller::MainController* mainController) :
    intervalPosition(0),
    samplesInInterval(0),
    mainController(mainController),
    metronomeTrackNode(createMetronomeTrackNode( mainController->getSampleRate())),
    lastBeat(0),
    currentBpi(0),
    currentBpm(0),
    mutex(QMutex::Recursive),
    encodersMutex(QMutex::Recursive),
    encodingThread(nullptr),
    preparedForTransmit(false),
    waitingIntervals(0) // waiting for start transmit
{
    running = false;

}

ninjam::User NinjamController::getUserByName(const QString &userName) const
{
    auto server = mainController->getNinjamService()->getCurrentServer();
    auto users = server->getUsers();
    for (const auto &user : users) {
        if (user.getName() == userName)
            return user;
    }
    return ninjam::User();
}

void NinjamController::setBpm(int newBpm)
{
    currentBpm = newBpm;
    samplesInInterval = computeTotalSamplesInInterval();
    metronomeTrackNode->setSamplesPerBeat(getSamplesPerBeat());

    emit currentBpmChanged(currentBpm);
}

void NinjamController::removeEncoder(int groupChannelIndex)
{
    QMutexLocker locker(&mutex);
    if (encoders.contains(groupChannelIndex)) {
        encoders.remove(groupChannelIndex);
    }
}

//+++++++++++++++++++++++++ THE MAIN LOGIC IS HERE  ++++++++++++++++++++++++++++++++++++++++++++++++

void NinjamController::process(const audio::SamplesBuffer &in, audio::SamplesBuffer &out, int sampleRate)
{

    QMutexLocker locker(&mutex);

    if (!running || samplesInInterval <= 0) {
        return; // not initialized
    }

    int totalSamplesToProcess = out.getFrameLenght();
    int samplesProcessed = 0;

    int offset = 0;

    do {
        emit startProcessing(intervalPosition); // vst host time line is updated with this event

        int samplesToProcessInThisStep = (std::min)((int)(samplesInInterval - intervalPosition), totalSamplesToProcess - offset);

        assert(samplesToProcessInThisStep);

        static audio::SamplesBuffer tempOutBuffer(out.getChannels(), samplesToProcessInThisStep);
        tempOutBuffer.setFrameLenght(samplesToProcessInThisStep);
        tempOutBuffer.zero();

        audio::SamplesBuffer tempInBuffer(in.getChannels(), samplesToProcessInThisStep);
        tempInBuffer.set(in, offset, samplesToProcessInThisStep, 0);

        bool newInterval = intervalPosition == 0;
        if (newInterval) { // starting new interval
            handleNewInterval();
        }

        metronomeTrackNode->setIntervalPosition(this->intervalPosition);
        int currentBeat = intervalPosition / getSamplesPerBeat();
        if (currentBeat != lastBeat) {
            lastBeat = currentBeat;
            emit intervalBeatChanged(currentBeat);
        }

        //+++++++++++ MAIN AUDIO OUTPUT PROCESS +++++++++++++++
        bool isLastPart = intervalPosition + samplesToProcessInThisStep >= samplesInInterval;
        for (NinjamTrackNode* track : trackNodes) {
            track->setProcessingLastPartOfInterval(isLastPart); // TODO resampler still need a flag indicating the last part?
        }
        mainController->doAudioProcess(tempInBuffer, tempOutBuffer, sampleRate);
        out.add(tempOutBuffer, offset); // generate audio output
        //++++++++++++++++++++++++++++++++++++++++++++++++++++++

        if (preparedForTransmit) {
            // 1) mix input subchannels, 2) encode and 3) send the encoded audio
            bool isFirstPart = intervalPosition == 0;
            int groupedChannels = mainController->getInputTrackGroupsCount();
            for (int groupIndex = 0; groupIndex < groupedChannels; ++groupIndex) {
                if (mainController->isTransmiting(groupIndex)) {
                    int channels = mainController->getMaxAudioChannelsForEncoding(groupIndex);
                    if (channels > 0) {
                        audio::SamplesBuffer inputMixBuffer(channels, samplesToProcessInThisStep);

                        if (encoders.contains(groupIndex)) {
                            inputMixBuffer.zero();
                            mainController->mixGroupedInputs(groupIndex, inputMixBuffer);

                            // encoding is running in another thread to avoid slow down the audio thread
                            encodingThread->addSamplesToEncode( inputMixBuffer, groupIndex, isFirstPart, isLastPart);
                        }
                    }
                }
            }
        }

        samplesProcessed += samplesToProcessInThisStep;
        offset += samplesToProcessInThisStep;
        this->intervalPosition = (this->intervalPosition + samplesToProcessInThisStep) % samplesInInterval;
    }
    while (samplesProcessed < totalSamplesToProcess);
}


audio::MetronomeTrackNode* NinjamController::createMetronomeTrackNode(int sampleRate)
{
    audio::SamplesBuffer firstBeatBuffer(2);
    audio::SamplesBuffer offBeatBuffer(2);
    audio::SamplesBuffer accentBeatBuffer(2);
    if (!(mainController->isUsingCustomMetronomeSounds())) {
        QString builtInMetronomeAlias = mainController->getSettings().getBuiltInMetronome();
        audio::metronomeUtils::createBuiltInSounds(builtInMetronomeAlias, firstBeatBuffer, offBeatBuffer, accentBeatBuffer, sampleRate);
    }
    else {
        QString firstBeatAudioFile = mainController->getMetronomeFirstBeatFile();
        QString offBeatAudioFile = mainController->getMetronomeOffBeatFile();
        QString accentBeatAudioFile = mainController->getMetronomeOffBeatFile();
        audio::metronomeUtils::createCustomSounds(firstBeatAudioFile, offBeatAudioFile, accentBeatAudioFile, firstBeatBuffer, offBeatBuffer, accentBeatBuffer, sampleRate);
    }

    audio::metronomeUtils::removeSilenceInBufferStart(firstBeatBuffer);
    audio::metronomeUtils::removeSilenceInBufferStart(offBeatBuffer);
    audio::metronomeUtils::removeSilenceInBufferStart(accentBeatBuffer);

    return new audio::MetronomeTrackNode(firstBeatBuffer, offBeatBuffer, accentBeatBuffer);
}

void NinjamController::recreateMetronome(int newSampleRate)
{
    // remove the old metronome
    float oldGain = metronomeTrackNode->getGain();
    float oldPan = metronomeTrackNode->getPan();
    bool oldMutedStatus = metronomeTrackNode->isMuted();
    bool oldSoloStatus = metronomeTrackNode->isSoloed();
    QList<int> oldAccentBeats = metronomeTrackNode->getAccentBeats();

    mainController->removeTrack(METRONOME_TRACK_ID);

    // recreate metronome using the new sample rate
    this->metronomeTrackNode = createMetronomeTrackNode(newSampleRate);
    this->metronomeTrackNode->setSamplesPerBeat(getSamplesPerBeat());
    this->metronomeTrackNode->setGain( oldGain );
    this->metronomeTrackNode->setPan( oldPan );
    this->metronomeTrackNode->setMute( oldMutedStatus );
    this->metronomeTrackNode->setSolo( oldSoloStatus );
    this->metronomeTrackNode->setAccentBeats(oldAccentBeats);
    mainController->addTrack(METRONOME_TRACK_ID, this->metronomeTrackNode);
}


void NinjamController::stop(bool emitDisconnectedSignal)
{
    if (isRunning()) {
        this->running = false;

        // store metronome settings
        auto metronomeTrack = mainController->getTrackNode(METRONOME_TRACK_ID);
        if (metronomeTrack) {
            float metronomeGain = Utils::poweredGainToLinear(metronomeTrack->getGain());
            float metronomePan = metronomeTrack->getPan();
            bool metronomeIsMuted = metronomeTrack->isMuted();

            mainController->storeMetronomeSettings(metronomeGain, metronomePan, metronomeIsMuted);
            mainController->removeTrack(METRONOME_TRACK_ID);//remove metronome
        }

        // clear all tracks
        for (auto trackNode : trackNodes.values()) {
            mainController->removeTrack(trackNode->getID());
        }
        trackNodes.clear();
    }

    if (encodingThread) {
        encodingThread->stop();
        encodingThread->wait(); // wait the encoding thread to finish
        delete encodingThread;
        encodingThread = nullptr;
    }

    for (AudioEncoder* encoder : encoders.values()) {
        delete encoder;
    }
    encoders.clear();

    // delete possible non consumed events
    for (SchedulableEvent *e : scheduledEvents)
        delete e;
    
    scheduledEvents.clear();

    qCDebug(jtNinjamCore) << "NinjamController destructor - disconnecting...";

    auto ninjamService = mainController->getNinjamService();
    disconnect(ninjamService, &ninjam::Service::serverBpmChanged, this, &NinjamController::scheduleBpmChangeEvent);
    disconnect(ninjamService, &ninjam::Service::serverBpiChanged, this, &NinjamController::scheduleBpiChangeEvent);
    disconnect(ninjamService, &ninjam::Service::audioIntervalCompleted, this, &NinjamController::handleIntervalCompleted);

    disconnect(ninjamService, &ninjam::Service::userChannelCreated, this, &NinjamController::addNinjamRemoteChannel);
    disconnect(ninjamService, &ninjam::Service::userChannelRemoved, this, &NinjamController::removeNinjamRemoteChannel);
    disconnect(ninjamService, &ninjam::Service::userChannelUpdated, this, &NinjamController::updateNinjamRemoteChannel);
    disconnect(ninjamService, &ninjam::Service::audioIntervalDownloading, this, &NinjamController::handleIntervalDownloading);

    disconnect(ninjamService, &ninjam::Service::publicChatMessageReceived, this, &NinjamController::publicChatMessageReceived);
    disconnect(ninjamService, &ninjam::Service::privateChatMessageReceived, this, &NinjamController::privateChatMessageReceived);
    disconnect(ninjamService, &ninjam::Service::serverTopicMessageReceived, this, &NinjamController::topicMessageReceived);

    ninjamService->disconnectFromServer(emitDisconnectedSignal);
}

NinjamController::~NinjamController()
{
    //QMutexLocker locker(&mutex);
    qCDebug(jtNinjamCore) << "NinjamController destructor";
    if (isRunning()) {
        stop(false);
    }

    // delete possible non consumed events
    for (SchedulableEvent *e : scheduledEvents) {
        delete e;
    }

}

void NinjamController::start(const ninjam::Server& server)
{
    qCDebug(jtNinjamCore) << "starting ninjam controller...";
    QMutexLocker locker(&mutex);

    //schedule an update in internal attributes
    scheduledEvents.append(new BpiChangeEvent(this, server.getBpi()));
    scheduledEvents.append(new BpmChangeEvent(this, server.getBpm()));
    preparedForTransmit = false; // the xmit start after the first interval is received
    emit preparingTransmission();

    // schedule the encoders creation (one encoder for each channel)
    int channels = mainController->getInputTrackGroupsCount();
    for (int channelIndex = 0; channelIndex < channels; ++channelIndex) {
        scheduledEvents.append(new InputChannelChangedEvent(this, channelIndex));
    }

    processScheduledChanges();

    if (!running) {

        encodingThread = new NinjamController::EncodingThread(this);

        // add a sine wave generator as input to test audio transmission
        //mainController->addInputTrackNode(new Audio::LocalInputTestStreamer(440, mainController->getAudioDriverSampleRate()));


        mainController->addTrack(METRONOME_TRACK_ID, this->metronomeTrackNode);
        mainController->setTrackMute(METRONOME_TRACK_ID, mainController->getSettings().getMetronomeMuteStatus());
        mainController->setTrackGain(METRONOME_TRACK_ID,mainController->getSettings().getMetronomeGain());
        mainController->setTrackPan(METRONOME_TRACK_ID,  mainController->getSettings().getMetronomePan());

        this->intervalPosition  = lastBeat = 0;


        auto ninjamService = mainController->getNinjamService();
        connect(ninjamService, &ninjam::Service::serverBpmChanged, this, &NinjamController::scheduleBpmChangeEvent);
        connect(ninjamService, &ninjam::Service::serverBpiChanged, this, &NinjamController::scheduleBpiChangeEvent);
        connect(ninjamService, &ninjam::Service::audioIntervalCompleted, this, &NinjamController::handleIntervalCompleted);

        connect(ninjamService, &ninjam::Service::userChannelCreated, this, &NinjamController::addNinjamRemoteChannel);
        connect(ninjamService, &ninjam::Service::userChannelRemoved, this, &NinjamController::removeNinjamRemoteChannel);
        connect(ninjamService, &ninjam::Service::userChannelUpdated, this, &NinjamController::updateNinjamRemoteChannel);
        connect(ninjamService, &ninjam::Service::audioIntervalDownloading, this, &NinjamController::handleIntervalDownloading);
        connect(ninjamService, &ninjam::Service::userExited, this, &NinjamController::handleNinjamUserExiting);
        connect(ninjamService, &ninjam::Service::userEntered, this, &NinjamController::handleNinjamUserEntering);

        connect(ninjamService, &ninjam::Service::publicChatMessageReceived, this, &NinjamController::handleReceivedPublicChatMessage);
        connect(ninjamService, &ninjam::Service::privateChatMessageReceived, this, &NinjamController::handleReceivedPrivateChatMessage);
        connect(ninjamService, &ninjam::Service::serverTopicMessageReceived, this, &NinjamController::topicMessageReceived);

        // add tracks for users connected in server
        auto users = server.getUsers();
        for (const auto &user : users) {
            for (const auto &channel : user.getChannels()) {
                addTrack(user, channel);
            }
        }

        this->running = true;

        emit started();
    }
    qCDebug(jtNinjamCore) << "ninjam controller started!";
}

void NinjamController::blockUserInChat(const ninjam::User &user)
{
    QString uniqueKey = getUniqueKeyForUser(user);
    if (!chatBlockedUsers.contains(uniqueKey)) {
        chatBlockedUsers.append(uniqueKey);
        emit userBlockedInChat(user.getName());
    }
}

void NinjamController::blockUserInChat(const QString &userNameToBlock)
{
    blockUserInChat(getUserByName(userNameToBlock));
}

void NinjamController::unblockUserInChat(const ninjam::User &user)
{
    QString uniqueKey = getUniqueKeyForUser(user);
    if (chatBlockedUsers.removeOne(uniqueKey))
        emit userUnblockedInChat(user.getName());
}

void NinjamController::unblockUserInChat(const QString &userNameToBlock)
{
    unblockUserInChat(getUserByName(userNameToBlock));
}

bool NinjamController::userIsBlockedInChat(const ninjam::User &user)
{
    QString uniqueKey = getUniqueKeyForUser(user);
    return chatBlockedUsers.contains(uniqueKey);
}

bool NinjamController::userIsBlockedInChat(const QString &userName) const
{
    return userIsBlockedInChat(getUserByName(userName));
}

void NinjamController::handleReceivedPublicChatMessage(const ninjam::User &user, const QString &message)
{
    if (!userIsBlockedInChat(user))
        emit publicChatMessageReceived(user, message);
}

void NinjamController::handleReceivedPrivateChatMessage(const ninjam::User &user, const QString &message)
{
    if (!userIsBlockedInChat(user))
        emit privateChatMessageReceived(user, message);
}

void NinjamController::sendChatMessage(const QString &msg)
{
    auto service = mainController->getNinjamService();

    if (chat::isAdminCommand(msg)) {
        service->sendAdminCommand(msg);
    }
    else if (chat::isPrivateMessage(msg)) {
        service->sendPrivateChatMessage(msg);
    }
    else {
        service->sendPublicChatMessage(msg);
    }
}

long NinjamController::generateNewTrackID()
{
    //QMutexLocker locker(&mutex);
    static long TRACK_IDS = 100;
    return TRACK_IDS++;
}

QString NinjamController::getUniqueKeyForChannel(const ninjam::UserChannel &channel)
{
    return channel.getUserFullName() + QString::number(channel.getIndex());
}

QString NinjamController::getUniqueKeyForUser(const ninjam::User &user)
{
    return user.getFullName(); // full name is 'user_name@IP'
}

bool NinjamController::userIsBot(const QString userName) const
{
    if (mainController) {
        return mainController->getBotNames().contains(userName);
    }
    return false;
}

void NinjamController::addTrack(const ninjam::User &user, const ninjam::UserChannel &channel)
{
    if (userIsBot(user.getName())) {
        return;
    }

    auto trackNode = new NinjamTrackNode(generateNewTrackID());

    bool trackAdded = false;

    //checkThread("addTrack();");
    {
        QMutexLocker locker(&mutex);
        trackNodes.insert(getUniqueKeyForChannel(channel), trackNode);
    } // release the mutex before emit the signal
    
    trackAdded = mainController->addTrack(trackNode->getID(), trackNode);

    if (trackAdded) {
        emit channelAdded(user,  channel, trackNode->getID());
    }
    else {
        QMutexLocker locker(&mutex);
        trackNodes.remove(getUniqueKeyForChannel(channel));
        delete trackNode;
    }
}

void NinjamController::removeTrack(const ninjam::User &user, const ninjam::UserChannel &channel)
{
    bool channelDeleted = false;
    long ID = -1;
    {
        QMutexLocker locker(&mutex);
        //checkThread("removeTrack();");
        QString uniqueKey = getUniqueKeyForChannel(channel);

        if (trackNodes.contains(uniqueKey)) {
            auto trackNode = trackNodes[uniqueKey];
            ID = trackNode->getID();
            trackNodes.remove(uniqueKey);
            mainController->removeTrack(ID);
            channelDeleted = true;
        }
    }
    
    if (channelDeleted) {
        emit channelRemoved(user, channel, ID);
    }
}

void NinjamController::voteBpi(int bpi)
{
    mainController->getNinjamService()->voteToChangeBPI(bpi);
}

void NinjamController::voteBpm(int bpm)
{
    mainController->getNinjamService()->voteToChangeBPM(bpm);
}

void NinjamController::setMetronomeBeatsPerAccent(int beatsPerAccent, int currentBpi)
{
    metronomeTrackNode->setBeatsPerAccent(beatsPerAccent, currentBpi);
}

QList<int> NinjamController::getMetronomeAccentBeats()
{
    return metronomeTrackNode->getAccentBeats();
}

void NinjamController::setMetronomeAccentBeats(QList<int> accentBeats)
{
    metronomeTrackNode->setAccentBeats(accentBeats);
}

//void NinjamController::deleteDeactivatedTracks(){
//    //QMutexLocker locker(&mutex);
//    checkThread("deleteDeactivatedTracks();");
//    QList<QString> keys = trackNodes.keys();
//    foreach (QString key, keys) {
//        NinjamTrackNode* trackNode = trackNodes[key];
//        if(!(trackNode->isActivated())){
//            trackNodes.remove(key);
//            mainController->removeTrack(trackNode->getID());
//            //delete trackNode; //BUG - sometimes Jamtaba crash when trackNode is deleted
//        }
//    }
//}

void NinjamController::handleNewInterval()
{
    // check if the transmiting can start
    if (!preparedForTransmit) {
        if(waitingIntervals >= TOTAL_PREPARED_INTERVALS){
            preparedForTransmit = true;
            waitingIntervals = 0;
            emit preparedToTransmit();
        }
        else {
            waitingIntervals++;
        }
    }

    if (hasScheduledChanges()) {
        processScheduledChanges();
    }
    
    //QMutexLocker locker(&mutex);
    for (NinjamTrackNode* track : trackNodes.values()) {
        bool trackWasPlaying = track->isPlaying();
        bool trackIsPlaying = track->startNewInterval();
        if (trackWasPlaying != trackIsPlaying) {
            emit channelXmitChanged(track->getID(), trackIsPlaying);
        }
    }

    emit startingNewInterval(); // update the UI

    mainController->syncWithNinjamIntervalStart(samplesInInterval);
}

void NinjamController::processScheduledChanges()
{
    for (SchedulableEvent* event : scheduledEvents) {
        event->process();
        delete event;
    }
    scheduledEvents.clear();
}

long NinjamController::getSamplesPerBeat()
{
    return computeTotalSamplesInInterval()/currentBpi;
}

long NinjamController::computeTotalSamplesInInterval()
{
    double intervalPeriod =  60000.0 / currentBpm * currentBpi;
    return (long)(mainController->getSampleRate() * intervalPeriod / 1000.0);
}

//ninjam slots
void NinjamController::handleNinjamUserEntering(const ninjam::User &user)
{
    emit userEnter(user.getName());
}

void NinjamController::handleNinjamUserExiting(const ninjam::User &user)
{
     for (const auto &channel : user.getChannels()) {
        removeTrack(user, channel);
     }
     emit userLeave(user.getName());
}

void NinjamController::addNinjamRemoteChannel(const ninjam::User &user, const ninjam::UserChannel &channel)
{
    addTrack(user, channel);
}

void NinjamController::removeNinjamRemoteChannel(const ninjam::User &user, const ninjam::UserChannel &channel)
{
    removeTrack(user, channel);
}

void NinjamController::updateNinjamRemoteChannel(const ninjam::User &user, const ninjam::UserChannel &channel)
{
    QString uniqueKey = getUniqueKeyForChannel(channel);
    QMutexLocker locker(&mutex);
    if (trackNodes.contains(uniqueKey)) {
        auto trackNode = trackNodes[uniqueKey];
        emit channelNameChanged(user, channel, trackNode->getID());
    }

}

void NinjamController::scheduleBpiChangeEvent(quint16 newBpi, quint16 oldBpi)
{
    Q_UNUSED(oldBpi);
    scheduledEvents.append(new BpiChangeEvent(this, newBpi));
}

void NinjamController::scheduleBpmChangeEvent(quint16 newBpm)
{
    Q_UNUSED(newBpm)
    scheduledEvents.append(new BpmChangeEvent(this, newBpm));
}

void NinjamController::handleIntervalCompleted(const ninjam::User &user, quint8 channelIndex, const QByteArray &encodedData)
{
    if (mainController->isMultiTrackRecordingActivated()) {
        auto geoLocation = mainController->getGeoLocation(user.getIp());
        QString userName = user.getName() + " from " + geoLocation.getCountryName();
        mainController->saveEncodedAudio(userName, channelIndex, encodedData);
    }

    auto channel = user.getChannel(channelIndex);
    QString channelKey = getUniqueKeyForChannel(channel);
    QMutexLocker locker(&mutex);
    if (trackNodes.contains(channelKey)) {
        NinjamTrackNode* trackNode = trackNodes[channelKey];
        if (trackNode) {
            trackNode->addVorbisEncodedInterval(encodedData);
            emit channelAudioFullyDownloaded(trackNode->getID());
        }
    }
    else {
        qWarning() << "The channel " << channelIndex << " of user " << user.getName() << " not founded in map!";
    }
}

void NinjamController::reset(bool keepRecentIntervals)
{
    QMutexLocker locker(&mutex);
    for (NinjamTrackNode* trackNode : trackNodes.values()) {
        trackNode->discardDownloadedIntervals(keepRecentIntervals);
    }
    intervalPosition = lastBeat = 0;
}

void NinjamController::scheduleEncoderChangeForChannel(int channelIndex)
{
    scheduledEvents.append(new InputChannelChangedEvent(this, channelIndex));
}

QByteArray NinjamController::encode(const audio::SamplesBuffer &buffer, uint channelIndex)
{
    QMutexLocker locker(&encodersMutex);
    if (encoders.contains(channelIndex)) {
        return encoders[channelIndex]->encode(buffer);
    }
    return QByteArray();
}

QByteArray NinjamController::encodeLastPartOfInterval(uint channelIndex)
{
    QMutexLocker locker(&encodersMutex);
    if (encoders.contains(channelIndex)) {
        return encoders[channelIndex]->finishIntervalEncoding();
    }
    return QByteArray();
}

void NinjamController::recreateEncoderForChannel(int channelIndex)
{

    QMutexLocker locker(&encodersMutex);
    int maxChannelsForEncoding = mainController->getMaxAudioChannelsForEncoding(channelIndex);

    if (maxChannelsForEncoding <= 0) // input track is setted as noInput?
        return;

    bool currentEncoderIsInvalid = encoders.contains(channelIndex) &&
            (encoders[channelIndex]->getChannels() != maxChannelsForEncoding
                || encoders[channelIndex]->getSampleRate() != mainController->getSampleRate());

    if (!encoders.contains(channelIndex) || currentEncoderIsInvalid) { // a new encoder is necessary?

        if (currentEncoderIsInvalid && encoders.contains(channelIndex)) {
            delete encoders[channelIndex];
        }

        int sampleRate = mainController->getSampleRate();
        float encodingQuality = mainController->getEncodingQuality();

        encoders[channelIndex] = new vorbis::Encoder(maxChannelsForEncoding, sampleRate, encodingQuality);
    }
}

void NinjamController::recreateEncoders()
{
    if (isRunning()) {
        QMutexLocker locker(&encodersMutex); // this method is called from main thread, and the encoders are used in audio thread every time
        for (int e = 0; e < encoders.size(); ++e) {
            delete encoders[e];
        }
        encoders.clear(); // new encoders will be create on demand

        int trackGroupsCount = mainController->getInputTrackGroupsCount();
        for (int channelIndex = 0; channelIndex < trackGroupsCount; ++channelIndex) {
            recreateEncoderForChannel(channelIndex);
        }
    }
}

void NinjamController::setSampleRate(int newSampleRate)
{
    if (!isRunning()) {
        return;
    }

    reset(false); // discard all downloaded intervals

    this->samplesInInterval = computeTotalSamplesInInterval();

    recreateMetronome(newSampleRate);

    recreateEncoders();
}

void NinjamController::handleIntervalDownloading(const ninjam::User &user, quint8 channelIndex, int downloadedBytes)
{
    Q_UNUSED(downloadedBytes);
    auto channel = user.getChannel(channelIndex);
    QString channelKey = getUniqueKeyForChannel(channel);

    mutex.lock();
    NinjamTrackNode* track = trackNodes[channelKey];
    mutex.unlock();

    if (track) {
        if (!track->isPlaying()) { // track is not playing yet and receive the first interval bytes
            emit channelXmitChanged(track->getID(), true);
        }
        emit channelAudioChunkDownloaded(track->getID());
    }
}
