#ifndef TRACKGROUPVIEW_H
#define TRACKGROUPVIEW_H

#include <QFrame>
#include <QLineEdit>
#include <QBoxLayout>
#include <QGridLayout>

#include "TextEditorModifier.h"

namespace Ui {
class TrackGroupView;
}

class BaseTrackView;
class QMenu;

class TrackGroupView : public QFrame
{
    Q_OBJECT

public:
    explicit TrackGroupView(TextEditorModifier *TextEditorModifier, QWidget *parent = 0);
    virtual ~TrackGroupView();
    virtual void setGroupName(const QString &groupName);
    virtual QString getGroupName() const;

    virtual BaseTrackView *addTrackView(long trackID);

    void removeTrackView(BaseTrackView *trackView);
    void removeTrackView(int index);

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

    virtual void updateGuiElements();

    int getTracksCount() const;

    void setUnlightStatus(bool unlighted);
    bool isUnlighted() const;

    // is not possible return a covariant container, so I'm using template to return a container of a more specific (derived) type
    template<class T>
    QList<T> getTracks() const
    {
        QList<T> castedTracks;
        foreach (BaseTrackView *trackView, trackViews) {
            castedTracks.append(dynamic_cast<T>(trackView));
        }
        return castedTracks;
    }

    void setTintColor(const QColor &color);

protected:

    void changeEvent(QEvent *) override;

    virtual void translateUi();

    virtual void populateContextMenu(QMenu &contextMenu);

    QList<BaseTrackView *> trackViews;

    virtual BaseTrackView *createTrackView(long trackID) = 0;

    QLineEdit *groupNameField;
    QWidget *topPanel;
    QBoxLayout *tracksLayout;
    QBoxLayout *topPanelLayout;
    QGridLayout *mainLayout;

    virtual void refreshStyleSheet();

private slots:
    void showContextMenu(const QPoint &pos);
    void showPeakMeterOnly();
    void showRmsOnly();
    void showPeakAndRms();
    void showMaxPeakMarker(bool showMarker);

private:
    void setupUI(TextEditorModifier *textEditorFactory);
};


inline int TrackGroupView::getTracksCount() const
{
    return trackViews.size();
}

#endif // TRACKGROUPVIEW_H
