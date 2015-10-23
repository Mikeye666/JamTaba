#pragma once

#include "SamplesBuffer.h"
#include <QObject>
#include <QMutex>

namespace Controller {
    class MainController;
}


namespace Audio{

class ChannelRange{

private:
    int firstChannel;
    int channelsCount;
public:
    ChannelRange(int firstChannel, int channelsCount);
    ChannelRange();
    inline int getChannels() const{return channelsCount;}
    inline bool isMono() const{return channelsCount == 1;}
    void setToStereo();
    void setToMono();
    inline int getFirstChannel() const{return firstChannel;}
    inline int getLastChannel() const{return firstChannel + channelsCount - 1;}
    inline bool isEmpty() const{return getChannels() <= 0;}
};

//class AudioDriverListener{
//public:
//    virtual void process(const Audio::SamplesBuffer& in, Audio::SamplesBuffer& out, int sampleRate) = 0;
//    virtual ~AudioDriverListener();
//protected:
//    AudioDriverListener(){}
//};

class AudioDriver : public QObject
{
    Q_OBJECT

signals:
    void sampleRateChanged(int newSampleRate);//invocado quando acontece alguma mudança na configuração de buffer size, por exemplo
    void stopped();
    void started();

public:
    explicit AudioDriver( Controller::MainController* mainController);
	virtual ~AudioDriver();
    virtual void setProperties(int deviceIndex, int firstIn, int lastIn, int firstOut, int lastOut, int sampleRate, int bufferSize);
    virtual void setProperties(int inputDeviceIndex, int outputDeviceIndex, int firstIn, int lastIn, int firstOut, int lastOut, int sampleRate, int bufferSize);
    virtual void setProperties(int sampleRate, int bufferSize);//used in mac

    virtual void stop() = 0;
    virtual void start() = 0;
    virtual void release() = 0;

    inline ChannelRange getSelectedInputs() const{return globalInputRange;}
    inline ChannelRange getSelectedOutputs() const{return globalOutputRange;}

    virtual inline int getSampleRate() const {return sampleRate;}
    virtual inline int getBufferSize() const {return bufferSize;}

    virtual QList<int> getValidSampleRates(int deviceIndex) const = 0;
    virtual QList<int> getValidBufferSizes(int deviceIndex) const = 0;

    virtual int getMaxInputs() const = 0;//return max inputs for an audio device. My fast track ultra (8 channels) return 8, etc.
    virtual int getMaxOutputs() const = 0;

    virtual const char* getInputChannelName(unsigned const int index) const = 0;
    virtual const char* getOutputChannelName(unsigned const int index) const = 0;

    virtual const char* getInputDeviceName(int index) const = 0;
    inline const char* getInputDeviceName() const{return getInputDeviceName(inputDeviceIndex);}

    virtual const char* getOutputDeviceName(int index) const = 0;

    virtual int getInputDeviceIndex() const = 0;
    virtual void setInputDeviceIndex(int index) = 0;
    virtual int getOutputDeviceIndex() const = 0;
    virtual void setOutputDeviceIndex(int index) = 0;

    virtual int getDevicesCount() const = 0;

    const SamplesBuffer& getOutputBuffer() const {return *outputBuffer;}
protected:
    ChannelRange globalInputRange;//the range of input channels selected in audio preferences menu
    ChannelRange globalOutputRange;//the range of output channels selected in audio preferences menu

    int inputDeviceIndex;//index of selected device index. In ASIO the inputDeviceIndex and outputDeviceIndex are equal.
    int outputDeviceIndex;

    int sampleRate;
    int bufferSize;

    QScopedPointer<SamplesBuffer> inputBuffer;
    QScopedPointer<SamplesBuffer> outputBuffer;

    void recreateBuffers();

    Controller::MainController* mainController;
};

class NullAudioDriver : public AudioDriver{
public:
    NullAudioDriver()
        :AudioDriver(nullptr){

    }
    inline void stop(){}
    inline void start(){}
    inline void release(){}
    inline QList<int> getValidBufferSizes(int ) const{return QList<int>();}
    inline QList<int> getValidSampleRates(int ) const{return QList<int>();}
    inline int getMaxInputs() const{return 0;}
    inline int getMaxOutputs() const{return 0;}
    inline const char* getInputChannelName(const unsigned int ) const{return "";}
    inline const char* getOutputChannelName(const unsigned int ) const{return "";}
    inline const char* getInputDeviceName(int) const{return "NullAudioDriver";}
    inline const char* getOutputDeviceName(int) const{return "NullAudioDriver";}
    inline int getInputDeviceIndex() const{return -1;}
    inline int getOutputDeviceIndex() const{return -1;}
    inline void setInputDeviceIndex(int ){}
    inline void setOutputDeviceIndex(int){}
    inline int getDevicesCount() const{return 0;}
};

}