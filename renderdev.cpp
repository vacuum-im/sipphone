#include "renderdev.h"

#include <QThread>
#include <QMetaObject>
#include <QMutexLocker>
#include <QCoreApplication>

#include <pj/log.h>
#include <pj/assert.h>

#define DEFAULT_FPS         25
#define DEFAULT_WIDTH       640
#define DEFAULT_HEIGHT      480
#define DEFAULT_CLOCK_RATE  90000

/************************************************************************/
/* Frame Format Definitions                                             */
/************************************************************************/
struct qwidget_format 
{
	pj_uint32_t              pj_fmt;
	QImage::Format           qt_fmt;
};

static struct qwidget_format qwidget_formats[] = {
	{ PJMEDIA_FORMAT_BGRA,  QImage::Format_ARGB32 }
};

static QImage::Format get_qimage_format(pj_uint32_t pj_fmt)
{
	int count = PJ_ARRAY_SIZE(qwidget_formats);
	for (int i=0; i<count; i++)
		if (qwidget_formats[i].pj_fmt = pj_fmt)
			return qwidget_formats[i].qt_fmt;
	return QImage::Format_Invalid;
}

/************************************************************************/
/* VideoSurface                                                         */
/************************************************************************/

VideoSurface::VideoSurface()
{
	FFrameKey = 0;
	FFrameSize = QSize(DEFAULT_WIDTH,DEFAULT_HEIGHT);
}

VideoSurface::~VideoSurface()
{
	emit surfaceDestroyed();
}

QSize VideoSurface::frameSize() const
{
	return FFrameSize;
}

qint64 VideoSurface::frameKey() const
{
	return FFrameKey;
}

bool VideoSurface::putFrame(const pjmedia_frame *AFrame)
{
	QMutexLocker locker(&FMutex);
	if (FFormat != QImage::Format_Invalid)
	{
		if (AFrame)
			FFrame = QImage((uchar *)AFrame->buf, FFrameSize.width(), FFrameSize.height(), FFormat).copy();
		else
			FFrame = QImage();
		FFrameKey = FFrame.cacheKey();
		emit frameChanged();
	}
	return true;
}

bool VideoSurface::setFormat(const pjmedia_format *AFormat)
{
	QImage::Format format = get_qimage_format(AFormat->id);
	if (format != QImage::Format_Invalid)
	{
		QMutexLocker locker(&FMutex);
		FFormat = format;
		FFrameSize = QSize(AFormat->det.vid.size.w,AFormat->det.vid.size.h);
		emit formatChanged();
		return true;
	}
	return false;
}

void VideoSurface::paint(QPainter *APainter, const QRect &ATarget)
{
	QMutexLocker locker(&FMutex);
	if (!FFrame.isNull())
		APainter->drawImage(ATarget,FFrame);
	else
		APainter->fillRect(ATarget,Qt::transparent);
}


/************************************************************************/
/* VideoWindow                                                          */
/************************************************************************/
VideoWindow::VideoWindow(QWidget *AParent) : QWidget(AParent)
{
	setAutoFillBackground(false);
	setAttribute(Qt::WA_PaintOnScreen, true);
	setAttribute(Qt::WA_NoSystemBackground, true);
	
	FSurface = NULL;
	FCurFrameKey = -1;
	FSizeHint = QSize(DEFAULT_WIDTH,DEFAULT_HEIGHT);
}

VideoWindow::~VideoWindow()
{
	emit windowDestroyed();
}

QSize VideoWindow::sizeHint() const
{
	return FSizeHint;
}

void VideoWindow::paintEvent(QPaintEvent *AEvent)
{
	Q_UNUSED(AEvent);
	QPainter p(this);
	if (FSurface != NULL)
		FSurface->paint(&p,rect());
	else
		p.fillRect(rect(),Qt::transparent);
}

VideoSurface *VideoWindow::surface() const
{
	return FSurface;
}

void VideoWindow::setSurface(VideoSurface *ASurface)
{
	if (FSurface != ASurface)
	{
		if (FSurface)
		{
			disconnect(FSurface,SIGNAL(frameChanged()),this,SLOT(onFrameChanged()));
			disconnect(FSurface,SIGNAL(formatChanged()),this,SLOT(onFormatChanged()));
			disconnect(FSurface,SIGNAL(surfaceDestroyed()),this,SLOT(onSurfaceDestroyed()));
		}

		FSurface = ASurface;

		if (ASurface)
		{
			connect(FSurface,SIGNAL(frameChanged()),SLOT(onFrameChanged()),Qt::QueuedConnection);
			connect(FSurface,SIGNAL(formatChanged()),SLOT(onFormatChanged()),Qt::QueuedConnection);
			connect(FSurface,SIGNAL(surfaceDestroyed()),SLOT(onSurfaceDestroyed()));
		}

		onFormatChanged();
		onFrameChanged();
	}
}

void VideoWindow::onFrameChanged()
{
	qint64 newKey = FSurface!=NULL ? FSurface->frameKey() : 0;
	if (FCurFrameKey != newKey)
	{
		update();
		FCurFrameKey = newKey;
	}
}

void VideoWindow::onFormatChanged()
{
	if (FSurface)
		FSizeHint = FSurface->frameSize();
	updateGeometry();
}

void VideoWindow::onSurfaceDestroyed()
{
	setSurface(NULL);
}

/************************************************************************/
/* Stream Operations                                                    */
/************************************************************************/
struct qwidget_stream 
{
	pjmedia_vid_dev_stream           base;
	
	pj_pool_t                       *pool;
	pjmedia_vid_dev_param            param;
	pjmedia_video_apply_fmt_param    vafp;

	VideoSurface                    *surface;
	pj_bool_t                        is_running;
};

static pj_status_t qwidget_stream_get_param(pjmedia_vid_dev_stream *strm, pjmedia_vid_dev_param *param)
{
	PJ_ASSERT_RETURN(strm && param, PJ_EINVAL);

	struct qwidget_stream *qstrm = (struct qwidget_stream *)strm;
	pj_memcpy(param, &qstrm->param, sizeof(*param));

	if (strm->op->get_cap(strm, PJMEDIA_VID_DEV_CAP_FORMAT, &param->fmt) == PJ_SUCCESS)
		param->flags |= PJMEDIA_VID_DEV_CAP_FORMAT;

	return PJ_SUCCESS;
}

static pj_status_t qwidget_stream_get_cap(pjmedia_vid_dev_stream *strm, pjmedia_vid_dev_cap cap, void *value)
{
	PJ_ASSERT_RETURN(strm && value,PJ_EINVAL);
	struct qwidget_stream *qstrm = (struct qwidget_stream *)strm;

	switch (cap)
	{
	case PJMEDIA_VID_DEV_CAP_FORMAT:
		{
			pjmedia_format *format = (pjmedia_format *)value;
			pjmedia_format_copy(format,&qstrm->param.fmt);
		}
		return PJ_SUCCESS;
	default:
		return PJMEDIA_EVID_INVCAP;
	}
}

static pj_status_t qwidget_stream_set_cap(pjmedia_vid_dev_stream *strm, pjmedia_vid_dev_cap cap, const void *value)
{
	PJ_ASSERT_RETURN(strm && value, PJ_EINVAL);
	struct qwidget_stream *qstrm = (struct qwidget_stream *)strm;

	switch (cap)
	{
	case PJMEDIA_VID_DEV_CAP_FORMAT:
		{
			const pjmedia_format *format = (const pjmedia_format *)value;

			const pjmedia_video_format_info *vfi = pjmedia_get_video_format_info(pjmedia_video_format_mgr_instance(),format->id);
			if (vfi == NULL)
				return PJMEDIA_EVID_BADFORMAT;

			qstrm->vafp.buffer = NULL;
			qstrm->vafp.size = format->det.vid.size;
			if (vfi->apply_fmt(vfi, &qstrm->vafp) != PJ_SUCCESS)
				return PJMEDIA_EVID_BADFORMAT;

			if (!qstrm->surface->setFormat(format))
				return PJMEDIA_EVID_BADFORMAT;

			pjmedia_format_copy(&qstrm->param.fmt,format);
		}
		return PJ_SUCCESS;
	case PJMEDIA_VID_DEV_CAP_OUTPUT_HIDE:
		// Error in pjsua_vid.c:create_vid_win
		return PJ_SUCCESS;
	default:
		return PJMEDIA_EVID_INVCAP;
	}
}

static pj_status_t qwidget_stream_put_frame(pjmedia_vid_dev_stream *strm, const pjmedia_frame *frame)
{
	PJ_ASSERT_RETURN(strm && frame, PJ_EINVAL);
	struct qwidget_stream *qstrm = (struct qwidget_stream *)strm;

	if (!qstrm->is_running)
		return PJ_EINVALIDOP;

	if (frame->size==0 || frame->buf==NULL || frame->size<qstrm->vafp.framebytes)
		return PJ_SUCCESS;

	if (!qstrm->surface->putFrame(frame))
		return PJMEDIA_EVID_ERR;

	return PJ_SUCCESS;
}

static pj_status_t qwidget_stream_start(pjmedia_vid_dev_stream *strm)
{
	PJ_ASSERT_RETURN(strm, PJ_EINVAL);
	struct qwidget_stream *qstrm = (struct qwidget_stream *)strm;

	if (!qstrm->is_running)
	{
		qstrm->is_running = PJ_TRUE;
		PJ_LOG(4,(__FILE__,"QWidget factory stream started"));
	}

	return PJ_SUCCESS;
}

static pj_status_t qwidget_stream_stop(pjmedia_vid_dev_stream *strm)
{
	PJ_ASSERT_RETURN(strm, PJ_EINVAL);
	struct qwidget_stream *qstrm = (struct qwidget_stream *)strm;

	if (qstrm->is_running)
	{
		qstrm->is_running = PJ_FALSE;
		qstrm->surface->putFrame(NULL);
		PJ_LOG(4,(__FILE__,"QWidget factory stream stopped"));
	}

	return PJ_SUCCESS;
}

static pj_status_t qwidget_stream_destroy(pjmedia_vid_dev_stream *strm)
{
	PJ_ASSERT_RETURN(strm, PJ_EINVAL);

	struct qwidget_stream *qstrm = (struct qwidget_stream *)strm;
	strm->op->stop(strm);

	QMetaObject::invokeMethod(qstrm->surface,"deleteLater",Qt::QueuedConnection);
	qstrm->surface = NULL;

	pj_pool_release(qstrm->pool);

	PJ_LOG(4,(__FILE__,"QWidget factory stream destroyed"));
	return PJ_SUCCESS;
}

static pjmedia_vid_dev_stream_op qwidget_stream_op =
{
	&qwidget_stream_get_param,
	&qwidget_stream_get_cap,
	&qwidget_stream_set_cap,
	&qwidget_stream_start,
	NULL,
	&qwidget_stream_put_frame,
	&qwidget_stream_stop,
	&qwidget_stream_destroy
};


/************************************************************************/
/* Factory Operations                                                   */
/************************************************************************/
struct qwidget_factory 
{
	pjmedia_vid_dev_factory     base;

	pj_pool_t                  *pool;
	pj_pool_factory		         *pf;

	pjmedia_vid_dev_info	      info;
};

static pj_status_t qwidget_factory_init(pjmedia_vid_dev_factory *f)
{
	struct qwidget_factory *qf = (struct qwidget_factory*)f;

	pj_bzero(&qf->info,sizeof(qf->info));
	strncpy(qf->info.name,QT_RENDER_DEVICE_NAME,sizeof(qf->info.name));
	strncpy(qf->info.driver,QT_RENDER_DRIVER_NAME,sizeof(qf->info.driver));
	qf->info.dir = PJMEDIA_DIR_RENDER;
	qf->info.has_callback = PJ_FALSE;
	qf->info.caps =	PJMEDIA_VID_DEV_CAP_FORMAT;

	qf->info.fmt_cnt = PJ_ARRAY_SIZE(qwidget_formats);
	for (unsigned int j = 0; j<qf->info.fmt_cnt; j++)
	{
		pjmedia_format *fmt = &qf->info.fmt[j];
		pjmedia_format_init_video(fmt,qwidget_formats[j].pj_fmt,DEFAULT_WIDTH,DEFAULT_HEIGHT,DEFAULT_FPS,1);
	}

	PJ_LOG(4,(__FILE__,"QWidget factory initialized"));
	return PJ_SUCCESS;
}

static pj_status_t qwidget_factory_destroy(pjmedia_vid_dev_factory *f)
{
	struct qwidget_factory *qf = (struct qwidget_factory*)f;

	pj_pool_release(qf->pool);

	PJ_LOG(4,(__FILE__,"QWidget factory destroyed"));
	return PJ_SUCCESS;
}

static unsigned qwidget_factory_get_dev_count(pjmedia_vid_dev_factory *f)
{
	PJ_UNUSED_ARG(f);
	return 1;
}

static pj_status_t qwidget_factory_get_dev_info(pjmedia_vid_dev_factory *f, unsigned index, pjmedia_vid_dev_info *info)
{
	PJ_ASSERT_RETURN(index < 1, PJMEDIA_EVID_INVDEV);

	struct qwidget_factory *qf = (struct qwidget_factory*)f;
	pj_memcpy(info, &qf->info, sizeof(*info));

	return PJ_SUCCESS;
}

static pj_status_t qwidget_factory_default_param(pj_pool_t *pool, pjmedia_vid_dev_factory *f, unsigned index, pjmedia_vid_dev_param *param)
{
	PJ_UNUSED_ARG(pool);
	PJ_ASSERT_RETURN(index < 1, PJMEDIA_EVID_INVDEV);

	struct qwidget_factory *qf = (struct qwidget_factory*)f;

	pj_bzero(param, sizeof(*param));
	param->dir = PJMEDIA_DIR_RENDER;
	param->cap_id = PJMEDIA_VID_INVALID_DEV;
	param->rend_id = index;

	param->flags = PJMEDIA_VID_DEV_CAP_FORMAT;
	param->fmt.type = PJMEDIA_TYPE_VIDEO;
	param->clock_rate = DEFAULT_CLOCK_RATE;
	pj_memcpy(&param->fmt,&qf->info.fmt[0],sizeof(param->fmt));

	return PJ_SUCCESS;
}

static pj_status_t qwidget_factory_create_stream(pjmedia_vid_dev_factory *f, pjmedia_vid_dev_param *param, const pjmedia_vid_dev_cb *cb, void *user_data, pjmedia_vid_dev_stream **p_vid_strm)
{
	PJ_UNUSED_ARG(cb); PJ_UNUSED_ARG(user_data);
	PJ_ASSERT_RETURN(param->dir==PJMEDIA_DIR_RENDER, PJ_EINVAL);

	struct qwidget_factory *qf = (struct qwidget_factory*)f;
	pj_pool_t *pool = pj_pool_create(qf->pf,"qwidget-stream-pool",1024,1024,NULL);
	PJ_ASSERT_RETURN(pool!=NULL, PJ_ENOMEM);

	struct qwidget_stream *qstrm = PJ_POOL_ZALLOC_T(pool, struct qwidget_stream);
	pj_bzero(qstrm,sizeof(*qstrm));
	qstrm->base.op = &qwidget_stream_op;

	qstrm->pool = pool;
	pj_memcpy(&qstrm->param,param,sizeof(*param));
	qstrm->param.flags = 0;

	qstrm->surface = new VideoSurface();
	qstrm->surface->moveToThread(QCoreApplication::instance()->thread());

	qstrm->param.window.type = (pjmedia_vid_dev_hwnd_type)QT_RENDER_VID_DEV_TYPE;
	qstrm->param.window.info.window = qstrm->surface;

	if (param->flags & PJMEDIA_VID_DEV_CAP_FORMAT)
		qwidget_stream_set_cap(&qstrm->base,PJMEDIA_VID_DEV_CAP_FORMAT,&param->fmt);

	*p_vid_strm = &qstrm->base;

	PJ_LOG(4,(__FILE__,"QWidget factory stream created"));
	return PJ_SUCCESS;
}

static pj_status_t qwidget_factory_refresh(pjmedia_vid_dev_factory *f)
{
	PJ_UNUSED_ARG(f);
	return PJ_SUCCESS;
}

static pjmedia_vid_dev_factory_op qwidget_factory_op =
{
	&qwidget_factory_init,
	&qwidget_factory_destroy,
	&qwidget_factory_get_dev_count,
	&qwidget_factory_get_dev_info,
	&qwidget_factory_default_param,
	&qwidget_factory_create_stream,
	&qwidget_factory_refresh
};

pjmedia_vid_dev_factory* qwidget_factory_create(pj_pool_factory *pf)
{
	pj_pool_t *pool = pj_pool_create(pf, "qwidget-factory-pool", 1024, 1024, NULL);
	struct qwidget_factory *factory = PJ_POOL_ZALLOC_T(pool, struct qwidget_factory);

	factory->pf = pf;
	factory->pool = pool;
	factory->base.op = &qwidget_factory_op;

	PJ_LOG(4,(__FILE__,"QWidget factory created"));
	return &factory->base;
}
