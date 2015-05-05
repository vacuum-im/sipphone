#include "sipworker.h"

#include <QMetaType>
#include <QMetaObject>

// SipTask
quint32 SipTask::FTaskCount = 0;
SipTask::SipTask(Type AType)
{
	FType = AType;
	FStatus = -1;
	FTaskId = QString("SipTask_%1").arg(++FTaskCount);

	setAutoDelete(false);
}

SipTask::~SipTask()
{

}

SipTask::Type SipTask::type() const
{
	return FType;
}

QString SipTask::taskId() const
{
	return FTaskId;
}

pj_status_t SipTask::status() const
{
	return FStatus;
}

// SipTaskCreateStack
SipTaskCreateStack::SipTaskCreateStack(const Params &AParams) : SipTask(CreateStack)
{
	FParams = AParams;
}

void SipTaskCreateStack::run()
{
	FStatus = pjsua_create();
	if (FStatus == PJ_SUCCESS)
	{
		// PJSUA Configuration
		pjsua_config uc;
		pjsua_config_default(&uc);

		QByteArray stun = FParams.stun.toLocal8Bit();
		if (!stun.isEmpty())
		{
			uc.stun_srv_cnt = 1;
			uc.stun_srv[0] = pj_str(stun.data());
		}

		QByteArray userAgent = FParams.userAgent.toLocal8Bit();
		uc.user_agent = pj_str(userAgent.data());

		uc.cb = FParams.callBack;

		// PJSUA Logging Configuration
		pjsua_logging_config lc;
		pjsua_logging_config_default(&lc);
		QByteArray logFileName = FParams.logFileName.toLocal8Bit();
		lc.log_filename = pj_str(logFileName.data());
		lc.msg_logging = PJ_TRUE;

		// PJSUA Media Configuration
		pjsua_media_config mc;
		pjsua_media_config_default(&mc);
		mc.enable_ice = FParams.enableIce ? PJ_TRUE : PJ_FALSE;

		FStatus = pjsua_init(&uc, &lc, &mc);
		if (FStatus == PJ_SUCCESS)
		{
			pjsua_transport_id uid,tid;
			pjsua_transport_config utc,ttc;

			pjsua_transport_config_default(&utc);
			pjsua_transport_config_default(&ttc);

			utc.port = FParams.udpPort;
			pj_status_t status_udp = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &utc, &uid);
			if (status_udp!=PJ_SUCCESS && utc.port!=0)
			{
				utc.port = 0;
				status_udp = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &utc, &uid);
			}

			ttc.port = FParams.tcpPort;
			pj_status_t status_tcp = pjsua_transport_create(PJSIP_TRANSPORT_TCP, &ttc, &tid);
			if (status_tcp!=PJ_SUCCESS && ttc.port!=0)
			{
				ttc.port = 0;
				status_tcp = pjsua_transport_create(PJSIP_TRANSPORT_TCP, &ttc, &tid);
			}

			FStatus = status_udp!=PJ_SUCCESS ? status_udp : status_tcp;

			if (FStatus == PJ_SUCCESS)
			{
				FStatus = pjmedia_vid_register_factory(FParams.vdf,NULL);
				if (FStatus == PJ_SUCCESS)
				{
					FStatus = pjsua_start();
					if (FStatus == PJ_SUCCESS)
						pjsua_dump(PJ_FALSE);
				}
			}
		}

		if (FStatus != PJ_SUCCESS)
			pjsua_destroy();
	}
}

// SipTaskDestroyStack
SipTaskDestroyStack::SipTaskDestroyStack() : SipTask(DestroyStack)
{

}

void SipTaskDestroyStack::run()
{
	FStatus = pjsua_destroy();
}

// SipTaskStartPreview
SipTaskStartPreview::SipTaskStartPreview(int ACapDev, int ARendDev) : SipTask(StartPreview)
{
	FCapDev = ACapDev;
	FRendDev = ARendDev;

	FWindow = NULL;
	FWindowType = PJMEDIA_VID_DEV_HWND_TYPE_NONE;
}

int SipTaskStartPreview::captureDev() const
{
	return FCapDev;
}

int SipTaskStartPreview::renderDev() const
{
	return FRendDev;
}

void *SipTaskStartPreview::window() const
{
	return FWindow;
}

int SipTaskStartPreview::windowType() const
{
	return FWindowType;
}

void SipTaskStartPreview::run()
{
	pjsua_vid_preview_param pp;
	pjsua_vid_preview_param_default(&pp);
	pp.show = PJ_FALSE;
	pp.rend_id = FRendDev;

	FStatus = pjsua_vid_preview_start(FCapDev,&pp);
	if (FStatus == PJ_SUCCESS)
	{
		pjsua_vid_win_info wi;
		FStatus = pjsua_vid_win_get_info(pjsua_vid_preview_get_win(FCapDev),&wi);
		if (FStatus == PJ_SUCCESS)
		{
			FWindow = wi.hwnd.info.window;
			FWindowType = wi.hwnd.type;
		}
		else
		{
			pjsua_vid_preview_stop(FCapDev);
		}
	}
}


// SipTaskStopPreview
SipTaskStopPreview::SipTaskStopPreview(int ACapDev) : SipTask(StopPreview)
{
	FCapDev = ACapDev;
}

int SipTaskStopPreview::captureDev() const
{
	return FCapDev;
}

void SipTaskStopPreview::run()
{
	FStatus = pjsua_vid_preview_stop(FCapDev);
}

// SipWorker
SipWorker::SipWorker(QObject *AParent) : QThread(AParent)
{
	FQuit = false;
	qRegisterMetaType<SipTask *>("SipTask *");
	start();
}

SipWorker::~SipWorker()
{
	quit();
	wait();
}

bool SipWorker::startTask(SipTask *ATask)
{
	QMutexLocker locker(&FMutex);
	if (!FQuit)
	{
		FTasks.enqueue(ATask);
		FTaskReady.wakeAll();
		return true;
	}
	return false;
}

void SipWorker::quit()
{
	QMutexLocker locker(&FMutex);
	FQuit = true;
	FTaskReady.wakeAll();
}

void SipWorker::run()
{
	QMutexLocker locker(&FMutex);
	while (!FQuit || !FTasks.isEmpty())
	{
		SipTask *task = !FTasks.isEmpty() ? FTasks.dequeue() : NULL;
		if (task)
		{
			locker.unlock();
			task->run();
			QMetaObject::invokeMethod(this,"taskFinished",Qt::QueuedConnection,Q_ARG(SipTask *,task));
			locker.relock();
		}
		else
		{
			FTaskReady.wait(locker.mutex());
		}
	}
}
