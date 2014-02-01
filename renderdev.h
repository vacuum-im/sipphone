#ifndef RENDERDEV_H
#define RENDERDEV_H

#include <QImage>
#include <QMutex>
#include <QWidget>
#include <QPainter>
#include <pjmedia.h>
#include <pjmedia_videodev.h>

#define QT_RENDER_DRIVER_NAME    "Qt"
#define QT_RENDER_DEVICE_NAME    "QWidget renderer"
#define QT_RENDER_VID_DEV_TYPE   485

class VideoSurface :
	public QObject
{
	Q_OBJECT;
public:
	VideoSurface();
	~VideoSurface();
	QSize frameSize() const;
	qint64 frameKey() const;
	bool putFrame(const pjmedia_frame *AFrame);
	bool setFormat(const pjmedia_format *AFormat);
	void paint(QPainter *APainter, const QRect &ATarget);
signals:
	void frameChanged();
	void formatChanged();
	void surfaceDestroyed();
private:
	QMutex FMutex;
	QImage FFrame;
	QSize FFrameSize;
	qint64 FFrameKey;
	QImage::Format FFormat;
};

class VideoWindow :
	public QWidget
{
	Q_OBJECT;
public:
	VideoWindow(QWidget *AParent = NULL);
	~VideoWindow();
	QSize sizeHint() const;
	VideoSurface *surface() const;
	void setSurface(VideoSurface *ASurface);
signals:
	void windowDestroyed();
protected:
	void paintEvent(QPaintEvent *AEvent);
protected slots:
	void onFrameChanged();
	void onFormatChanged();
	void onSurfaceDestroyed();
private:
	QSize FSizeHint;
	qint64 FCurFrameKey;
	VideoSurface *FSurface;
};

pjmedia_vid_dev_factory* qwidget_factory_create(pj_pool_factory *pf);

#endif // RENDERDEV_H
