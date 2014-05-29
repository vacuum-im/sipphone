#include "sipphone.h"

#include <QStringList>
#include <definitions/version.h>
#include <definitions/sipphone/optionvalues.h>
#include <utils/options.h>
#include <utils/logger.h>
#include <utils/jid.h>
#include "renderdev.h"

#define DEF_SIP_UDP_PORT              0
#define DEF_SIP_TCP_PORT              0
#define DEF_SIP_ICE_ENABLED           false
#define DEF_SIP_STUN_HOST             ""

SipPhone *SipPhone::FInstance = NULL;

SipPhone::SipPhone()
{
	FSipStackInited = false;
	FInstance = this;

	FSipWorker = new SipWorker(this);
	connect(FSipWorker,SIGNAL(taskFinished(SipTask *)),SLOT(onSipWorkerTaskFinished(SipTask *)));

	qRegisterMetaType<SipEvent *>("SipEvent *");
}

SipPhone::~SipPhone()
{
	delete FSipWorker;
	FInstance = NULL;
}

void SipPhone::pluginInfo(IPluginInfo *APluginInfo)
{
	APluginInfo->name = tr("SIP Phone");
	APluginInfo->description = tr("Allows to make voice and video calls over SIP protocol");
	APluginInfo->version = "1.0";
	APluginInfo->author = "Potapov S.A. aka Lion";
	APluginInfo->homePage = "http://www.vacuum-im.org";
}

bool SipPhone::initConnections(IPluginManager *APluginManager, int &AInitOrder)
{
	Q_UNUSED(AInitOrder);
	FPluginManager = APluginManager;

	connect(Options::instance(),SIGNAL(optionsOpened()),SLOT(onOptionsOpened()));
	connect(Options::instance(),SIGNAL(optionsClosed()),SLOT(onOptionsClosed()));

	return true;
}

bool SipPhone::initObjects()
{
	return true;
}

bool SipPhone::initSettings()
{
	Options::setDefaultValue(OPV_SIPPHONE_UPDPORT,DEF_SIP_UDP_PORT);
	Options::setDefaultValue(OPV_SIPPHONE_TCPPORT,DEF_SIP_TCP_PORT);
	Options::setDefaultValue(OPV_SIPPHONE_ICEENABLED,DEF_SIP_ICE_ENABLED);
	Options::setDefaultValue(OPV_SIPPHONE_STUNSERVER,QString(DEF_SIP_STUN_HOST));
	return true;
}

bool SipPhone::isCallsAvailable() const
{
	return FSipStackInited;
}

bool SipPhone::isAudioCallsAvailable() const
{
	return isCallsAvailable() && FAvailDevices.contains(ISipMedia::Audio);
}

bool SipPhone::isVideoCallsAvailable() const
{
	return isAudioCallsAvailable() && FAvailDevices.contains(ISipMedia::Video);
}

QList<ISipCall *> SipPhone::sipCalls(bool AActiveOnly) const
{
	FLock.lockForRead();
	QList<ISipCall *> calls;
	foreach(SipCall *call, FCalls)
	{
		if (!AActiveOnly || call->isActive())
			calls.append(call);
	}
	FLock.unlock();
	return calls;
}

ISipCall *SipPhone::newCall(const QUuid &AAccountId, const QString &ARemoteUri)
{
	if (isCallsAvailable() && FAccounts.contains(AAccountId))
	{
		if (pjsua_verify_sip_url(ARemoteUri.toLocal8Bit().constData())==PJ_SUCCESS || pjsua_verify_url(ARemoteUri.toLocal8Bit().constData())==PJ_SUCCESS)
		{
			LOG_INFO(QString("SIP call created as caller, call=%1, accId=%2, uri=%3").arg(-1).arg(AAccountId.toString(),ARemoteUri));
			SipCall *call = new SipCall(AAccountId,FAccounts.value(AAccountId),ARemoteUri,this);
			appendCall(call);
			return call;
		}
		else
		{
			LOG_ERROR(QString("Failed to create SIP call, accId=%1, uri=%2: Invalid SIP uri").arg(AAccountId.toString(),ARemoteUri));
		}
	}
	else if (!FAccounts.contains(AAccountId))
	{
		REPORT_ERROR("Failed to create SIP call: Account not found");
	}
	else if (!isCallsAvailable())
	{
		REPORT_ERROR("Failed to create SIP call: Calls is not available");
	}
	return NULL;
}

QList<QUuid> SipPhone::availAccounts() const
{
	return FAccounts.keys();
}

QString SipPhone::accountUri(const QUuid &AAccountId) const
{
	if (FAccounts.contains(AAccountId))
	{
		pjsua_acc_info accInfo;
		if (pjsua_acc_get_info(FAccounts.value(AAccountId),&accInfo) == PJ_SUCCESS)
			return QString::fromLocal8Bit(accInfo.acc_uri.ptr);
	}
	return QString::null;
}

ISipAccountConfig SipPhone::accountConfig(const QUuid &AAccountId) const
{
	ISipAccountConfig config;
	if (FAccounts.contains(AAccountId))
	{
		pjsua_acc_config accCfg;
		pj_pool_t *tmp_pool = pjsua_pool_create("tmp-acc-pool", 1024, 1024);
		if (pjsua_acc_get_config(FAccounts.value(AAccountId),tmp_pool,&accCfg)==PJ_SUCCESS && accCfg.cred_count>0)
		{
			config.userid = accCfg.id.ptr;
			config.userid.chop(1);
			config.userid.remove(0,5);

			config.password = accCfg.cred_info[0].data.ptr;

			if (accCfg.proxy_cnt > 0)
				parseSipUri(accCfg.proxy[0].ptr,config.proxyHost,config.proxyPort);
			parseSipUri(accCfg.reg_uri.ptr,config.serverHost,config.serverPort);
		}
		pj_pool_release(tmp_pool);
	}
	return config;
}

bool SipPhone::isAccountRegistered(const QUuid &AAccountId) const
{
	if (FAccounts.contains(AAccountId))
	{
		pjsua_acc_info accInfo;
		if (pjsua_acc_get_info(FAccounts.value(AAccountId),&accInfo) == PJ_SUCCESS)
			return accInfo.expires>0;
	}
	return false;
}

bool SipPhone::setAccountRegistered(const QUuid &AAccountId, bool ARegistered)
{
	if (FAccounts.contains(AAccountId) && isAccountRegistered(AAccountId)!=ARegistered)
	{
		pj_status_t status = pjsua_acc_set_registration(FAccounts.value(AAccountId), ARegistered ? PJ_TRUE : PJ_FALSE);
		if (status == PJ_SUCCESS)
		{
			LOG_INFO(QString("SIP account registration request sent, accId=%1, register=%2").arg(AAccountId.toString()).arg(ARegistered));
			return true;
		}
		else
		{
			LOG_ERROR(QString("Failed to send SIP account registration request, accId=%1: %2").arg(AAccountId.toString()).arg(resolveSipError(status)));
		}
	}
	return false;
}

bool SipPhone::insertAccount(const QUuid &AAccountId, const ISipAccountConfig &AConfig)
{
	if (FSipStackInited && !AAccountId.isNull() && !FAccounts.contains(AAccountId) && isValidConfig(AConfig))
	{
		pjsua_acc_config accCfg;
		if (parseConfig(AConfig, accCfg))
		{
			pjsua_acc_id accIndex;
			pj_status_t status = pjsua_acc_add(&accCfg, PJ_FALSE, &accIndex);
			if (status == PJ_SUCCESS)
			{
				LOG_INFO(QString("SIP account inserted, accId=%1, accIdx=%2").arg(AAccountId.toString()).arg(accIndex));
				FAccounts.insert(AAccountId,accIndex);
				emit accountInserted(AAccountId);
				return true;
			}
			else
			{
				LOG_ERROR(QString("Failed to create SIP account, accId=%1: %2").arg(AAccountId.toString()).arg(resolveSipError(status)));
			}
		}
		else
		{
			LOG_ERROR(QString("Failed to create SIP account, accId=%1: Config not parsed").arg(AAccountId.toString()));
		}
	}
	else if (!FSipStackInited)
	{
		LOG_ERROR(QString("Failed to create SIP account, accId=%1: SIP stack not initialized").arg(AAccountId.toString()));
	}
	else if (AAccountId.isNull())
	{
		LOG_ERROR(QString("Failed to create SIP account, accId=%1: Account Id is null").arg(AAccountId.toString()));
	}
	else if (FAccounts.contains(AAccountId))
	{
		LOG_ERROR(QString("Failed to create SIP account, accId=%1: Account Id already exists").arg(AAccountId.toString()));
	}
	else if (!isValidConfig(AConfig))
	{
		LOG_ERROR(QString("Failed to create SIP account, accId=%1: Invalid config").arg(AAccountId.toString()));
	}
	return false;
}

bool SipPhone::updateAccount(const QUuid &AAccountId, const ISipAccountConfig &AConfig)
{
	if (FAccounts.contains(AAccountId) && isValidConfig(AConfig))
	{
		pjsua_acc_config accCfg;
		if (parseConfig(AConfig, accCfg))
		{
			pjsua_acc_id accIndex = FAccounts.value(AAccountId);
			pj_status_t status = pjsua_acc_modify(accIndex, &accCfg);
			if (status == PJ_SUCCESS)
			{
				LOG_INFO(QString("SIP account updated, accId=%1").arg(AAccountId.toString()));
				emit accountChanged(AAccountId);
				return true;
			}
			else
			{
				LOG_ERROR(QString("Failed to update SIP account, accId=%1: %2").arg(AAccountId.toString()).arg(resolveSipError(status)));
			}
		}
		else
		{
			LOG_ERROR(QString("Failed to update SIP account, accId=%1: Config not parsed").arg(AAccountId.toString()));
		}
	}
	else if (!FAccounts.contains(AAccountId))
	{
		LOG_ERROR(QString("Failed to update SIP account, accId=%1: Account not found").arg(AAccountId.toString()));
	}
	else if (!isValidConfig(AConfig))
	{
		LOG_ERROR(QString("Failed to update SIP account, accId=%1: Invalid config").arg(AAccountId.toString()));
	}
	return false;
}

void SipPhone::removeAccount(const QUuid &AAccountId)
{
	if (FAccounts.contains(AAccountId))
	{
		LOG_INFO(QString("Removing SIP account, accId=%1").arg(AAccountId.toString()));

		qDeleteAll(findCallsByAccount(AAccountId));
		setAccountRegistered(AAccountId,false);

		pjsua_acc_del(FAccounts.take(AAccountId));

		emit accountRemoved(AAccountId);
	}
}

bool SipPhone::updateAvailDevices()
{
	if (FSipStackInited && pjsua_call_get_count()==0 && FVideoPreviewWidgets.isEmpty())
	{
		QMultiMap<int,ISipDevice> devices;

		unsigned numAudDevices = 64;
		pjmedia_aud_dev_info audDevInfo[64];
		if (pjmedia_aud_dev_refresh()==PJ_SUCCESS && pjsua_enum_aud_devs(audDevInfo, &numAudDevices)==PJ_SUCCESS)
		{
			LOG_DEBUG(QString("Found %1 SIP audio devices").arg(numAudDevices));
			for (unsigned devIndex = 0; devIndex < numAudDevices; devIndex++)
			{
				ISipDevice device;
				device.type = ISipMedia::Audio;
				device.index = devIndex;
				device.name = QString::fromLocal8Bit(audDevInfo[devIndex].name);
				device.formats = parseMediaFormats(audDevInfo[devIndex].ext_fmt,audDevInfo[devIndex].ext_fmt_cnt,ISipMedia::Audio);
				
				if (audDevInfo[devIndex].input_count>0 && audDevInfo[devIndex].output_count>0)
					device.dir = ISipMedia::CaptureAndPlayback;
				else if (audDevInfo[devIndex].input_count > 0)
					device.dir = ISipMedia::Capture;
				else if (audDevInfo[devIndex].output_count > 0)
					device.dir = ISipMedia::Playback;

				devices.insertMulti(device.type,device);

				QStringList formats;
				for (int i=0; i<device.formats.count(); i++)
				{
					char fid_str[5] = {'L','1','6',' ','\0' };
					const ISipMediaFormat &format = device.formats.at(i);
					if (format.id > 0)
						memcpy(fid_str,&format.id,4);
					formats.append(fid_str);
				}

				LOG_DEBUG(QString("  dev_i %1: %2 (driver=%3, in=%4, out=%5, fmt=%6) %7").arg(devIndex).arg(QString::fromLocal8Bit(audDevInfo[devIndex].name),QString::fromLocal8Bit(audDevInfo[devIndex].driver)).arg(audDevInfo[devIndex].input_count).arg(audDevInfo[devIndex].output_count).arg(device.formats.count()).arg(formats.join(", ")));
			}
		}
		else
		{
			LOG_ERROR("Failed to update SIP audio devices");
		}

		unsigned numVidDevices = 64;
		pjmedia_vid_dev_info vidDevInfo[64];
		if (pjmedia_vid_dev_refresh()==PJ_SUCCESS && pjsua_vid_enum_devs(vidDevInfo, &numVidDevices)==PJ_SUCCESS)
		{
			LOG_DEBUG(QString("Found %1 SIP video devices").arg(numVidDevices));
			for (unsigned devIndex = 0; devIndex < numVidDevices; devIndex++)
			{
				ISipDevice device;
				device.type = ISipMedia::Video;
				device.index = devIndex;
				device.name = QString::fromLocal8Bit(vidDevInfo[devIndex].name);
				device.formats = parseMediaFormats(vidDevInfo[devIndex].fmt,vidDevInfo[devIndex].fmt_cnt,ISipMedia::Video);

				if (vidDevInfo[devIndex].fmt_cnt > 0)
				{
					if (vidDevInfo[devIndex].dir == PJMEDIA_DIR_CAPTURE_PLAYBACK)
						device.dir = ISipMedia::CaptureAndPlayback;
					else if (vidDevInfo[devIndex].dir == PJMEDIA_DIR_CAPTURE)
						device.dir = ISipMedia::Capture;
					else if (vidDevInfo[devIndex].dir == PJMEDIA_DIR_PLAYBACK)
						device.dir = ISipMedia::Playback;

					devices.insertMulti(device.type,device);
				}

				QStringList formats;
				for (int i=0; i<device.formats.count(); i++)
				{
					char fid_str[5] = {'L','1','6',' ','\0' };
					const ISipMediaFormat &format = device.formats.at(i);
					if (format.id > 0)
						memcpy(fid_str,&format.id,4);
					formats.append(fid_str);
				}

				LOG_DEBUG(QString("  .dev_i %1: %2 (driver=%3, dir=%4, fmt=%5) %6").arg(devIndex).arg(QString::fromLocal8Bit(vidDevInfo[devIndex].name),QString::fromLocal8Bit(vidDevInfo[devIndex].driver)).arg(vidDevInfo[devIndex].dir).arg(device.formats.count()).arg(formats.join(", ")));
			}
		}
		else
		{
			LOG_ERROR("Failed to update SIP video devices");
		}

		if (FAvailDevices != devices)
		{
			FAvailDevices = devices;
			emit availDevicesChanged();
			return true;
		}
	}
	else if (!FSipStackInited)
	{
		LOG_ERROR("Failed to update avail SIP devices: SIP stack not initialize");
	}
	else if (pjsua_call_get_count() > 0)
	{
		LOG_ERROR("Failed to update avail SIP devices: Active calls found");
	}
	else if (FVideoPreviewWidgets.isEmpty())
	{
		LOG_ERROR("Failed to update avail SIP devices: Active preview widgets found");
	}
	return false;
}

ISipDevice SipPhone::findDevice(ISipMedia::Type AType, int AIndex) const
{
	for (QMap<int,ISipDevice>::const_iterator it=FAvailDevices.constBegin(); it!=FAvailDevices.constEnd(); it++)
		if (it.key()==AType && it->index==AIndex)
			return it.value();
	return ISipDevice();
}

ISipDevice SipPhone::findDevice(ISipMedia::Type AType, const QString &AName) const
{
	for (QMap<int,ISipDevice>::const_iterator it=FAvailDevices.constBegin(); it!=FAvailDevices.constEnd(); it++)
		if (it.key()==AType && it->name==AName)
			return it.value();
	return ISipDevice();
}

ISipDevice SipPhone::defaultDevice(ISipMedia::Type AType, ISipMedia::Direction ADir) const
{
	if (AType==ISipMedia::Video && ADir==ISipMedia::Playback)
		return findDevice(ISipMedia::Video,QT_RENDER_DEVICE_NAME);

	QList<ISipDevice> devices = availDevices(AType,ADir);
	qSort(devices.begin(),devices.end());

	return devices.value(0);
}

QList<ISipDevice> SipPhone::availDevices(ISipMedia::Type AType, ISipMedia::Direction ADir) const
{
	QList<ISipDevice> devices = FAvailDevices.values(AType);
	for (QList<ISipDevice>::iterator it=devices.begin(); it!=devices.constEnd(); )
	{
		if ((it->dir & ADir) != ADir)
			it = devices.erase(it);
		else
			++it;
	}
	return devices;
}

QWidget *SipPhone::startVideoPreview(const ISipDevice &ADevice, QWidget *AParent)
{
	VideoWindow *widget = NULL;
	if (ADevice.index>=0 && ADevice.type==ISipMedia::Video && (ADevice.dir & ISipMedia::Capture)>0 && FAvailDevices.values(ADevice.type).contains(ADevice))
	{
		LOG_DEBUG(QString("Starting SIP video preview, devIdx=%1, devName=%2").arg(ADevice.index).arg(ADevice.name));

		VideoSurface *surface = NULL;
		if (!FVideoPreviewWidgets.contains(ADevice.index))
		{
			SipTaskStartPreview *task = new SipTaskStartPreview(ADevice.index,defaultDevice(ISipMedia::Video,ISipMedia::Playback).index);
			if (FSipWorker->startTask(task))
				LOG_DEBUG(QString("SIP video preview start task started, devIdx=%1, devName=%2").arg(ADevice.index).arg(ADevice.name));
			else
				LOG_ERROR(QString("Failed to start SIP video preview start task, devIdx=%1, devName=%2").arg(ADevice.index).arg(ADevice.name));
		}
		else
		{
			surface = FVideoPreviewWidgets.value(ADevice.index)->surface();
		}
		
		widget = new VideoWindow(AParent);
		widget->setSurface(surface);
		connect(widget,SIGNAL(windowDestroyed()),SLOT(onVideoPreviewWidgetDestroyed()));
		FVideoPreviewWidgets.insertMulti(ADevice.index,widget);
	}
	else
	{
		LOG_ERROR(QString("Failed to start SIP video preview, devIdx=%1, devName=%2: Invalid device").arg(ADevice.index).arg(ADevice.name));
	}
	return widget;
}

void SipPhone::stopVideoPreview(QWidget *APreview)
{
	VideoWindow *widget = qobject_cast<VideoWindow *>(APreview);
	int devIndex = FVideoPreviewWidgets.key(widget,PJSUA_INVALID_ID);
	if (devIndex != PJSUA_INVALID_ID)
	{
		LOG_DEBUG(QString("Stopping SIP video preview, devIdx=%1").arg(devIndex));

		FVideoPreviewWidgets.remove(devIndex,widget);
		if (!FVideoPreviewWidgets.contains(devIndex))
		{
			SipTaskStopPreview *task = new SipTaskStopPreview(devIndex);
			if (FSipWorker->startTask(task))
				LOG_DEBUG(QString("SIP video preview stop task started, devIdx=%1").arg(devIndex));
			else
				LOG_ERROR(QString("Failed to start SIP video preview stop task, devIdx=%1").arg(devIndex));
		}
	}
	else
	{
		LOG_WARNING("Failed to stop SIP video preview: Device not found");
	}
}

QMultiMap<int,ISipCallHandler *> SipPhone::callHandlers() const
{
	return FCallHandlers;
}

void SipPhone::insertCallHandler(int AOrder, ISipCallHandler *AHandler)
{
	if (AHandler && !FCallHandlers.contains(AOrder,AHandler))
	{
		LOG_DEBUG(QString("SIP call handler inserted, order=%1, handler=%2").arg(AOrder).arg((quint64)AHandler));
		FCallHandlers.insertMulti(AOrder,AHandler);
	}
}

void SipPhone::removeCallHandler(int AOrder, ISipCallHandler *AHandler)
{
	if (FCallHandlers.contains(AOrder,AHandler))
	{
		LOG_DEBUG(QString("SIP call handler removed, order=%1, handler=%2").arg(AOrder).arg((quint64)AHandler));
		FCallHandlers.remove(AOrder,AHandler);
	}
}

void SipPhone::initSipStack()
{
	if (!FSipStackInited)
	{
		SipTaskCreateStack::Params params;
		params.stun = Options::node(OPV_SIPPHONE_STUNSERVER).value().toString();
		params.enableIce = Options::node(OPV_SIPPHONE_ICEENABLED).value().toBool();
		params.udpPort = Options::node(OPV_SIPPHONE_UPDPORT).value().toUInt();
		params.tcpPort = Options::node(OPV_SIPPHONE_TCPPORT).value().toUInt();
		params.userAgent = QString(CLIENT_NAME) + "/" + FPluginManager->version();
		//params.logFileName = QString(FPluginManager->homePath()+"/logs/pjsip.log");

		pj_bzero(&params.callBack, sizeof(params.callBack));
		params.callBack.on_reg_state = &pjcbOnRegState;
		params.callBack.on_nat_detect = &pjcbOnNatDetect;
		params.callBack.on_incoming_call = &pjcbOnIncomingCall;
		params.callBack.on_call_state = &pjcbOnCallState;
		params.callBack.on_call_media_state = &pjcbOnCallMediaState;
		params.callBack.on_call_media_event = &pjcbOnCallMediaEvent;

		params.vdf = &qwidget_factory_create;

		SipTaskCreateStack *task = new SipTaskCreateStack(params);
		if (FSipWorker->startTask(task))
			LOG_INFO(QString("Create SIP stack task started, stun='%1', ice=%2, udp=%3, tcp=%4, ua='%5'").arg(params.stun).arg(params.enableIce).arg(params.udpPort).arg(params.tcpPort).arg(params.userAgent));
		else
			LOG_ERROR("Failed to start create SIP stack task");
	}
}

void SipPhone::loadSipParams()
{
	if (FSipStackInited)
	{
		const pj_str_t h264Id = {"H264", 4};
		pjmedia_vid_codec_param h264Params;
		pjsua_vid_codec_get_param(&h264Id, &h264Params);
		h264Params.enc_fmt.det.vid.size.w = 640;
		h264Params.enc_fmt.det.vid.size.h = 480;
		h264Params.enc_fmt.det.vid.fps.num = 25;
		h264Params.enc_fmt.det.vid.fps.denum = 1;
		h264Params.enc_fmt.det.vid.avg_bps = 512000;
		h264Params.enc_fmt.det.vid.max_bps = 512000;
		pjsua_vid_codec_set_param(&h264Id, &h264Params);

		const pj_str_t h263Id = {"H263", 4};
		pjmedia_vid_codec_param h263Params;
		pjsua_vid_codec_get_param(&h263Id, &h263Params);
		h263Params.enc_fmt.det.vid.size.w = 640;
		h263Params.enc_fmt.det.vid.size.h = 480;
		h263Params.enc_fmt.det.vid.fps.num = 25;
		h263Params.enc_fmt.det.vid.fps.denum = 1;
		h263Params.enc_fmt.det.vid.avg_bps = 512000;
		h263Params.enc_fmt.det.vid.max_bps = 512000;
		pjsua_vid_codec_set_param(&h263Id, &h263Params);
	}
}

void SipPhone::destroySipStack()
{
	if (FSipStackInited)
	{
		LOG_INFO("Destroing SIP stack");

		foreach(const QString &sipid, FAccounts.keys())
			removeAccount(sipid);

		foreach(VideoWindow *widget, FVideoPreviewWidgets.values())
			delete widget;

		SipTaskDestroyStack *task = new SipTaskDestroyStack;
		if (FSipWorker->startTask(task))
			LOG_DEBUG("Destroy SIP stack task started");
		else
			LOG_ERROR("Failed to start destroy SIP stack task");
	}
}

QString SipPhone::resolveSipError(int ACode) const
{
	char errmsg[PJ_ERR_MSG_SIZE];
	pj_strerror(ACode, errmsg, sizeof(errmsg));
	return QString(errmsg);
}

bool SipPhone::isValidConfig(const ISipAccountConfig &AConfig) const
{
	char id_url[1030];
	pj_ansi_snprintf(id_url,sizeof(id_url),"<sip:%s>",AConfig.userid.toLocal8Bit().constData());
	return pjsua_verify_sip_url(id_url)==PJ_SUCCESS;
}

bool SipPhone::parseConfig(const ISipAccountConfig &ASrc, pjsua_acc_config &ADst) const
{
	pjsua_acc_config_default(&ADst);

	ADst.register_on_acc_add = PJ_FALSE;
	ADst.allow_via_rewrite = PJ_FALSE;
	ADst.allow_contact_rewrite = PJ_FALSE;

	Jid userId = ASrc.userid;
	QString serverHost = ASrc.serverHost.isEmpty() ? userId.domain() : ASrc.serverHost;

	static char id_url[1030];
	pj_ansi_snprintf(id_url,sizeof(id_url),"<sip:%s>",ASrc.userid.toLocal8Bit().constData());

	static char reguri[1030];
	if (ASrc.serverPort > 0)
		pj_ansi_snprintf(reguri,sizeof(reguri),"<sip:%s:%u>",serverHost.toLocal8Bit().constData(),ASrc.serverPort);
	else
		pj_ansi_snprintf(reguri,sizeof(reguri),"<sip:%s>",serverHost.toLocal8Bit().constData());

	static char username[512];
	pj_ansi_snprintf(username,sizeof(username),"%s",userId.node().toLocal8Bit().constData());

	static char password[512];
	pj_ansi_snprintf(password,sizeof(password),"%s",ASrc.password.toLocal8Bit().constData());

	ADst.id = pj_str(id_url);
	ADst.reg_uri = pj_str(reguri);

	ADst.vid_in_auto_show = PJ_FALSE;
	ADst.vid_out_auto_transmit = PJ_TRUE;
	ADst.vid_cap_dev = defaultDevice(ISipMedia::Video,ISipMedia::Capture).index;
	ADst.vid_rend_dev = defaultDevice(ISipMedia::Video,ISipMedia::Playback).index;

	ADst.cred_count = 1;
	ADst.cred_info[0].realm = pj_str((char*)"*");
	ADst.cred_info[0].scheme = pj_str((char*)"digest");
	ADst.cred_info[0].username = pj_str(username);
	ADst.cred_info[0].data = pj_str(password);
	ADst.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;

	if (!ASrc.proxyHost.isEmpty())
	{
		static char proxy[512];
		if (ASrc.proxyPort > 0)
			pj_ansi_snprintf(proxy,sizeof(proxy),"<sip:%s:%u>",ASrc.proxyHost.toLocal8Bit().constData(),ASrc.proxyPort);
		else
			pj_ansi_snprintf(proxy,sizeof(proxy),"<sip:%s>",ASrc.proxyHost.toLocal8Bit().constData());
		ADst.proxy[ADst.proxy_cnt++] = pj_str(proxy);
	}

	return true;
}

bool SipPhone::parseSipUri(const QString &AUri, QString &AAddress, quint16 &APort) const
{
	if (!AUri.isEmpty())
	{
		QString uri = AUri;
		if (uri.startsWith('<'))
		{
			uri.chop(1);
			uri.remove(0,1);
		}

		if (uri.startsWith("sip:"))
			uri.remove(0,4);

		if (!uri.isEmpty())
		{
			QStringList parts = uri.split(':',QString::SkipEmptyParts);
			AAddress = parts.value(0);
			APort = parts.value(0).toUInt();
			return true;
		}
	}
	return false;
}

QList<ISipMediaFormat> SipPhone::parseMediaFormats(pjmedia_format AFormats[], int ACount, int AType) const
{
	QList<ISipMediaFormat> formats;
	for (int i=0; i<ACount; i++)
	{
		if (AType == ISipMedia::Audio)
		{
			ISipMediaFormat format;
			format.type = ISipMedia::Audio;
			format.id = AFormats[i].id;
			format.details.aud.clockRate = AFormats[i].det.aud.clock_rate;
			format.details.aud.channelCount = AFormats[i].det.aud.channel_count;
			format.details.aud.frameTimeUsec = AFormats[i].det.aud.frame_time_usec;
			format.details.aud.bitsPerSample = AFormats[i].det.aud.bits_per_sample;
			format.details.aud.avgBitrate = AFormats[i].det.aud.avg_bps;
			format.details.aud.maxBitrate = AFormats[i].det.aud.max_bps;
			formats.append(format);
		}
		else if (AType == ISipMedia::Video)
		{
			ISipMediaFormat format;
			format.type = ISipMedia::Video;
			format.id = AFormats[i].id;
			format.details.vid.fpsNum = AFormats[i].det.vid.fps.num;
			format.details.vid.fpsDenum = AFormats[i].det.vid.fps.denum;
			format.details.vid.width = AFormats[i].det.vid.size.w;
			format.details.vid.height = AFormats[i].det.vid.size.h;
			format.details.vid.avgBitrate = AFormats[i].det.vid.avg_bps;
			format.details.vid.maxBitrate = AFormats[i].det.vid.max_bps;
			formats.append(format);
		}
	}
	return formats;
}

void SipPhone::appendCall(SipCall *ACall)
{
	if (ACall && !FCalls.contains(ACall))
	{
		FLock.lockForWrite();
		connect(ACall,SIGNAL(stateChanged()),SLOT(onSipCallStateChanged()));
		connect(ACall,SIGNAL(statusChanged()),SLOT(onSipCallStatusChanged()));
		connect(ACall,SIGNAL(mediaChanged()),SLOT(onSipCallMediaChanged()));
		connect(ACall,SIGNAL(callDestroyed()),SLOT(onSipCallDestroyed()));
		FCalls.append(ACall);
		FLock.unlock();
		emit callCreated(ACall);
	}
}

void SipPhone::removeCall(SipCall *ACall)
{
	if (FCalls.contains(ACall))
	{
		FLock.lockForWrite();
		FCalls.removeAll(ACall);
		FLock.unlock();
		emit callDestroyed(ACall);
	}
}

bool SipPhone::isDuplicateCall(pjsua_call_id ACallIndex) const
{
	pjsua_call_id calls[PJSUA_MAX_CALLS];
	unsigned callCount = sizeof(calls);
	pjsua_enum_calls(calls,&callCount);
	if (callCount > 1)
	{
		pjsua_call_info ci;
		if (pjsua_call_get_info(ACallIndex,&ci) == PJ_SUCCESS)
		{
			for (unsigned i=0; i<callCount; i++)
			{
				if (calls[i] != ACallIndex)
				{
					pjsua_call_info ci;
					pjsua_call_get_info(calls[i],&ci);
					if (pj_strcmp(&ci.call_id,&ci.call_id) == 0)
						return true;
				}
			}
		}
	}
	return false;
}

SipCall *SipPhone::findCallByIndex(pjsua_call_id ACallIndex) const
{
	SipCall *call = NULL;

	FLock.lockForRead();
	for (QList<SipCall *>::const_iterator it=FCalls.constBegin(); call==NULL && it!=FCalls.constEnd(); ++it)
		if ((*it)->callIndex() == ACallIndex)
			call = *it;
	FLock.unlock();

	return call;
}

QList<SipCall *> SipPhone::findCallsByAccount(const QUuid &AAccountId) const
{
	QList<SipCall *> calls;

	FLock.lockForRead();
	for (QList<SipCall *>::const_iterator it=FCalls.constBegin(); it!=FCalls.constEnd(); ++it)
		if ((*it)->accountId() == AAccountId)
			calls.append(*it);
	FLock.unlock();

	return calls;
}

void SipPhone::processSipEvent(SipEvent *AEvent)
{
	switch (AEvent->type)
	{
	case SipEvent::RegState:
		{
			SipEventRegState *se = static_cast<SipEventRegState *>(AEvent);

			QUuid accId = FAccounts.key(se->accIndex);
			if (!accId.isNull())
			{
				pjsua_acc_info ai;
				pjsua_acc_get_info(se->accIndex,&ai);
				bool registered = ai.expires>0;
				LOG_INFO(QString("SIP account registration changed, accId=%1, registered=%2, status=%3").arg(accId.toString()).arg(registered).arg(ai.status_text.ptr));
				emit accountRegistrationChanged(accId,registered);
			}

			delete se;
		}
		break;
	case SipEvent::IncomingCall:
		{
			SipEventIncomingCall *se = static_cast<SipEventIncomingCall *>(AEvent);

			QUuid accId = FAccounts.key(se->accIndex);
			if (!accId.isNull())
			{
				if (!isDuplicateCall(se->callIndex))
				{
					LOG_INFO(QString("SIP call created as receiver, call=%1, accId=%2").arg(se->callIndex).arg(accId.toString()));
					SipCall *call = new SipCall(accId,se->accIndex,se->callIndex,this);
					appendCall(call);

					bool callReceived = false;
					for (QMultiMap<int,ISipCallHandler *>::const_iterator it=FCallHandlers.constBegin(); !callReceived && it!=FCallHandlers.constEnd(); ++it)
						callReceived = it.value()->sipCallReceive(it.key(),call);

					if (!callReceived)
					{
						LOG_WARNING(QString("Incoming call not accepted, call=%1, accId=%2, uri=%3").arg(call->callIndex()).arg(call->accountId().toString()).arg(call->remoteUri()));
						call->hangupCall(ISipCall::SC_NotAcceptableHere);
						call->destroyCall(0);
					}
				}
				else
				{
					LOG_DEBUG(QString("Ignoring duplicate incoming call event: call=%1, accId=%2").arg(se->callIndex).arg(accId.toString()));
				}
			}
			else
			{
				LOG_WARNING(QString("Received call from unknown account, call=%1, accId=%2").arg(se->accIndex).arg(accId.toString()));
				pjsua_call_hangup(se->callIndex,PJSIP_SC_NOT_ACCEPTABLE_HERE,NULL,NULL);
			}

			delete se;
		}
		break;
	default:
		REPORT_ERROR(QString("Received unexpected SIP event: type=%1").arg(AEvent->type));
		delete AEvent;
	}
}

void SipPhone::onOptionsOpened()
{
	initSipStack();
}

void SipPhone::onOptionsClosed()
{
	destroySipStack();
}

void SipPhone::onSipCallDestroyed()
{
	SipCall *call = qobject_cast<SipCall *>(sender());
	if (call)
	{
		LOG_INFO(QString("SIP call destroyed, call=%1, accId=%2, uri=%3").arg(call->callIndex()).arg(call->accountId().toString(),call->remoteUri()));
		removeCall(call);
		if (findCallsByAccount(call->accountId()).isEmpty())
			setAccountRegistered(call->accountId(),false);
	}
}

void SipPhone::onSipCallStateChanged()
{
	SipCall *call = qobject_cast<SipCall *>(sender());
	if (call)
		emit callStateChanged(call);
}

void SipPhone::onSipCallStatusChanged()
{
	SipCall *call = qobject_cast<SipCall *>(sender());
	if (call)
		emit callStatusChanged(call);
}

void SipPhone::onSipCallMediaChanged()
{
	SipCall *call = qobject_cast<SipCall *>(sender());
	if (call)
		emit callMediaChanged(call);
}

void SipPhone::onVideoPreviewWidgetDestroyed()
{
	VideoWindow *widget = qobject_cast<VideoWindow *>(sender());
	stopVideoPreview(widget);
}

void SipPhone::onSipWorkerTaskFinished(SipTask *ATask)
{
	switch (ATask->type())
	{
	case SipTask::CreateStack:
		{
			SipTaskCreateStack *task = static_cast<SipTaskCreateStack *>(ATask);
			if (task->status() == PJ_SUCCESS)
			{
				LOG_INFO("SIP stack initialized");

				pj_thread_register("Qt GUI Thread",FPjThreadDesc,&FPjThread);
				FSipStackInited = true;

				loadSipParams();
				updateAvailDevices();

				emit callsAvailChanged(true);
			}
			else
			{
				LOG_ERROR(QString("Failed to initialize SIP stack: %1").arg(resolveSipError(task->status())));
			}
		}
		break;
	case SipTask::DestroyStack:
		{
			SipTaskCreateStack *task = static_cast<SipTaskCreateStack *>(ATask);

			FCalls.clear();
			FAccounts.clear();
			FAvailDevices.clear();
			FSipStackInited = false;

			if (task->status() == PJ_SUCCESS)
				LOG_INFO("SIP stack destroyed");
			else
				LOG_ERROR(QString("Failed to destroy SIP stack: %1").arg(resolveSipError(task->status())));

			emit callsAvailChanged(false);
		}
		break;
	case SipTask::StartPreview:
		{
			SipTaskStartPreview *task = static_cast<SipTaskStartPreview *>(ATask);
			if (task->status() == PJ_SUCCESS)
			{
				if (task->windowType() == QT_RENDER_VID_DEV_TYPE)
				{
					VideoSurface *surface = (VideoSurface *)task->window();
					foreach(VideoWindow *widget, FVideoPreviewWidgets.values(task->captureDev()))
						widget->setSurface(surface);
					LOG_DEBUG(QString("SIP video preview started, capIdx=%1, renIdx=%2").arg(task->captureDev()).arg(task->renderDev()));
				}
				else
				{
					LOG_ERROR(QString("Failed to start SIP video preview, capIdx=%1, renIdx=%2, winType=%3: Incompatible render device type").arg(task->captureDev()).arg(task->renderDev()).arg(task->windowType()));
				}
			}
			else
			{
				LOG_ERROR(QString("Failed to start SIP video preview, capIdx=%1, renIdx=%2: %3").arg(task->captureDev()).arg(task->renderDev()).arg(resolveSipError(task->status())));
			}
		}
		break;
	case SipTask::StopPreview:
		{
			SipTaskStopPreview *task = static_cast<SipTaskStopPreview *>(ATask);

			foreach(VideoWindow *widget, FVideoPreviewWidgets.values(task->captureDev()))
				widget->setSurface(NULL);

			if (task->status() == PJ_SUCCESS)
				LOG_DEBUG(QString("SIP video preview stopped, capIdx=%1").arg(task->captureDev()));
			else
				LOG_ERROR(QString("Failed to stop video preview, capIdx=%1: %2").arg(task->captureDev()).arg(resolveSipError(task->status())));
		}
		break;
	default:
		REPORT_ERROR(QString("Unexpected SIP task finished, type=%1").arg(ATask->type()));
		break;
	}
	delete ATask;
}

void SipPhone::pjcbOnRegState(pjsua_acc_id AAccIndex)
{
	SipEventRegState *se = new SipEventRegState;
	se->type = SipEvent::RegState;
	se->accIndex = AAccIndex;
	QMetaObject::invokeMethod(FInstance,"processSipEvent",Qt::QueuedConnection,Q_ARG(SipEvent *,se));
}

void SipPhone::pjcbOnNatDetect(const pj_stun_nat_detect_result *AResult)
{
	if (AResult->status == PJ_SUCCESS)
		LOG_INFO(QString("SIP NAT detected as %1").arg(AResult->nat_type_name));
	else
		LOG_ERROR(QString("Failed to detect NAT type: %1").arg(AResult->status));
}

void SipPhone::pjcbOnIncomingCall(pjsua_acc_id AAccIndex, pjsua_call_id ACallIndex, pjsip_rx_data *AData)
{
	Q_UNUSED(AData);
	SipEventIncomingCall *se = new SipEventIncomingCall;
	se->type = SipEvent::IncomingCall;
	se->accIndex = AAccIndex;
	se->callIndex = ACallIndex;
	QMetaObject::invokeMethod(FInstance,"processSipEvent",Qt::QueuedConnection,Q_ARG(SipEvent *,se));
}

void SipPhone::pjcbOnCallState(pjsua_call_id ACallIndex, pjsip_event *AEvent)
{
	Q_UNUSED(AEvent);
	SipCall *call = FInstance->findCallByIndex(ACallIndex);
	if (call)
		call->pjcbOnCallState();
}

void SipPhone::pjcbOnCallMediaState(pjsua_call_id ACallIndex)
{
	SipCall *call = FInstance->findCallByIndex(ACallIndex);
	if (call)
		call->pjcbOnCallMediaState();
}

void SipPhone::pjcbOnCallMediaEvent(pjsua_call_id ACallIndex, unsigned AMediaIndex, pjmedia_event *AEvent)
{
	SipCall *call = FInstance->findCallByIndex(ACallIndex);
	if (call)
		call->pjcbOnCallMediaEvent(AMediaIndex,AEvent);
}

Q_EXPORT_PLUGIN2(plg_sipphone, SipPhone)
