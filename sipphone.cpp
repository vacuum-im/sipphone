#include "sipphone.h"

#include <QStringList>
#include <definitions/version.h>
#include <definitions/sipphone/optionvalues.h>
#include <utils/options.h>
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
	APluginInfo->homePage = "http://www.qip.ru";
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
			SipCall *call = new SipCall(AAccountId,FAccounts.value(AAccountId),ARemoteUri,this);
			appendCall(call);
			return call;
		}
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
			config.userid = accCfg.cred_info[0].username.ptr;
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
			PJ_LOG(4,(__FILE__,"Account registration request sent: id=%s, reg=%d",AAccountId.toString().toLocal8Bit().constData(),ARegistered));
			return true;
		}
		else
		{
			PJ_LOG(1,(__FILE__,"Failed to send account registration request: id=%s, err=%d-%s",AAccountId.toString().toLocal8Bit().constData(),status,resolveSipError(status).toLocal8Bit().constData()));
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
				PJ_LOG(4,(__FILE__,"Account created: id=%s, accIndex=%d",AAccountId.toString().toLocal8Bit().constData(),accIndex));
				FAccounts.insert(AAccountId,accIndex);
				emit accountInserted(AAccountId);
				return true;
			}
			else
			{
				PJ_LOG(1,(__FILE__,"Failed to create account: id=%s, err=%d-%s",AAccountId.toString().toLocal8Bit().constData(),status,resolveSipError(status).toLocal8Bit().constData()));
			}
		}
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
				PJ_LOG(4,(__FILE__,"Account changed: id=%s, accIndex=%d",AAccountId.toString().toLocal8Bit().constData(),accIndex));
				emit accountChanged(AAccountId);
				return true;
			}
			else
			{
				PJ_LOG(1,(__FILE__,"Failed to change account: id=%s, err=%d-%s",AAccountId.toString().toLocal8Bit().constData(),status,resolveSipError(status).toLocal8Bit().constData()));
			}
		}
	}
	return false;
}

void SipPhone::removeAccount(const QUuid &AAccountId)
{
	if (FAccounts.contains(AAccountId))
	{
		qDeleteAll(findCallsByAccount(AAccountId));
		setAccountRegistered(AAccountId,false);

		PJ_LOG(4,(__FILE__,"Account destroyed: id=%s",AAccountId.toString().toLocal8Bit().constData()));
		pjsua_acc_del(FAccounts.value(AAccountId));
		FAccounts.remove(AAccountId);
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
			PJ_LOG(4,(__FILE__,"Found %d audio devices",numAudDevices));
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
				PJ_LOG(4,(__FILE__,".dev_i %d: %s (driver=%s, in=%d, out=%d, fmt=%d) %s",devIndex,audDevInfo[devIndex].name,audDevInfo[devIndex].driver,audDevInfo[devIndex].input_count,audDevInfo[devIndex].output_count,device.formats.count(),formats.join(", ").toLocal8Bit().constData()));
			}
		}

		unsigned numVidDevices = 64;
		pjmedia_vid_dev_info vidDevInfo[64];
		if (pjmedia_vid_dev_refresh()==PJ_SUCCESS && pjsua_vid_enum_devs(vidDevInfo, &numVidDevices)==PJ_SUCCESS)
		{
			PJ_LOG(4,(__FILE__,"Found %d video devices",numVidDevices));
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
				PJ_LOG(4,(__FILE__,".dev_i %d: %s (driver=%s, dir=%x, fmt=%d) %s",devIndex,vidDevInfo[devIndex].name,vidDevInfo[devIndex].driver,vidDevInfo[devIndex].dir,device.formats.count(),formats.join(", ").toLocal8Bit().constData()));
			}
		}

		if (FAvailDevices != devices)
		{
			FAvailDevices = devices;
			emit availDevicesChanged();
			return true;
		}
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
		VideoSurface *surface = NULL;
		if (!FVideoPreviewWidgets.contains(ADevice.index))
		{
			SipTaskStartPreview *task = new SipTaskStartPreview(ADevice.index,defaultDevice(ISipMedia::Video,ISipMedia::Playback).index);
			FSipWorker->startTask(task);
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
	return widget;
}

void SipPhone::stopVideoPreview(QWidget *APreview)
{
	VideoWindow *widget = qobject_cast<VideoWindow *>(APreview);
	int devIndex = FVideoPreviewWidgets.key(widget,PJSUA_INVALID_ID);
	if (devIndex != PJSUA_INVALID_ID)
	{
		FVideoPreviewWidgets.remove(devIndex,widget);
		if (!FVideoPreviewWidgets.contains(devIndex))
		{
			SipTaskStopPreview *task = new SipTaskStopPreview(devIndex);
			FSipWorker->startTask(task);
		}
	}
}

QMultiMap<int,ISipCallHandler *> SipPhone::callHandlers() const
{
	return FCallHandlers;
}

void SipPhone::insertCallHandler(int AOrder, ISipCallHandler *AHandler)
{
	if (AHandler && !FCallHandlers.contains(AOrder,AHandler))
		FCallHandlers.insertMulti(AOrder,AHandler);
}

void SipPhone::removeCallHandler(int AOrder, ISipCallHandler *AHandler)
{
	FCallHandlers.remove(AOrder,AHandler);
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
		params.logFileName = QString(FPluginManager->homePath()+"/logs/pjsip.log");

		pj_bzero(&params.callBack, sizeof(params.callBack));
		params.callBack.on_reg_state = &pjcbOnRegState;
		params.callBack.on_nat_detect = &pjcbOnNatDetect;
		params.callBack.on_incoming_call = &pjcbOnIncomingCall;
		params.callBack.on_call_state = &pjcbOnCallState;
		params.callBack.on_call_media_state = &pjcbOnCallMediaState;
		params.callBack.on_call_media_event = &pjcbOnCallMediaEvent;

		params.vdf = &qwidget_factory_create;

		SipTaskCreateStack *task = new SipTaskCreateStack(params);
		FSipWorker->startTask(task);
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
		foreach(const QString &sipid, FAccounts.keys())
			removeAccount(sipid);

		foreach(VideoWindow *widget, FVideoPreviewWidgets.values())
			delete widget;

		SipTaskDestroyStack *task = new SipTaskDestroyStack;
		FSipWorker->startTask(task);
	}
}

QString SipPhone::resolveSipError(int ACode)
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

	static char id_url[1030];
	pj_ansi_snprintf(id_url,sizeof(id_url),"<sip:%s>",ASrc.userid.toLocal8Bit().constData());

	static char reguri[1030];
	QString serverHost = ASrc.serverHost.isEmpty() ? ASrc.userid.mid(ASrc.userid.indexOf('@')+1) : ASrc.serverHost;
	if (ASrc.serverPort > 0)
		pj_ansi_snprintf(reguri,sizeof(reguri),"<sip:%s:%u>",serverHost.toLocal8Bit().constData(),ASrc.serverPort);
	else
		pj_ansi_snprintf(reguri,sizeof(reguri),"<sip:%s>",serverHost.toLocal8Bit().constData());

	static char username[512];
	pj_ansi_snprintf(username,sizeof(username),"%s",ASrc.userid.toLocal8Bit().constData());

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

			QString accId = FAccounts.key(se->accIndex);
			if (!accId.isEmpty())
			{
				pjsua_acc_info ai;
				pjsua_acc_get_info(se->accIndex,&ai);
				bool registered = ai.expires>0;
				PJ_LOG(4,(__FILE__,"Account registration changed: id=%s, status=%d-%s, expires=%d",accId.toLocal8Bit().constData(),ai.status,ai.status_text.ptr,ai.expires));
				emit accountRegistrationChanged(accId,registered);
			}

			delete se;
		}
		break;
	case SipEvent::IncomingCall:
		{
			SipEventIncomingCall *se = static_cast<SipEventIncomingCall *>(AEvent);

			QString accId = FAccounts.key(se->accIndex);
			if (!accId.isEmpty())
			{
				if (!isDuplicateCall(se->callIndex))
				{
					SipCall *call = new SipCall(accId,se->accIndex,se->callIndex,this);
					appendCall(call);

					bool callReceived = false;
					for (QMultiMap<int,ISipCallHandler *>::const_iterator it=FCallHandlers.constBegin(); !callReceived && it!=FCallHandlers.constEnd(); ++it)
						callReceived = it.value()->sipCallReceive(it.key(),call);

					if (!callReceived)
					{
						PJ_LOG(4,(__FILE__,"Destroying unhandled call, remoteUri=%s, accIndex=%d, callIndex=%d",call->remoteUri().toLocal8Bit().constData(),se->accIndex,se->callIndex));
						call->hangupCall(ISipCall::SC_NotAcceptableHere);
						call->destroyCall(0);
					}
				}
				else
				{
					PJ_LOG(4,(__FILE__,"Ignoring duplicate incoming call: callIndex=%d",se->callIndex));
				}
			}
			else
			{
				pjsua_call_hangup(se->callIndex,PJSIP_SC_NOT_ACCEPTABLE_HERE,NULL,NULL);
			}

			delete se;
		}
		break;
	default:
		PJ_LOG(1,(__FILE__,"Unhandled SipEvent: type=%s",AEvent->type));
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
				pj_thread_register("Qt GUI Thread",FPjThreadDesc,&FPjThread);
				FSipStackInited = true;

				loadSipParams();
				updateAvailDevices();

				PJ_LOG(4,(__FILE__,"Sip stack initialized"));
				emit callsAvailChanged(true);
			}
			else
			{
				PJ_LOG(1,(__FILE__,"Failed to initialize SIP stack: err=%d-%s",task->status(),resolveSipError(task->status()).toLocal8Bit().constData()));
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
				PJ_LOG(4,(__FILE__,"Sip stack destroyed"));
			else
				PJ_LOG(1,(__FILE__,"Failed to destroy SIP stack: err=%d-%s",task->status(),resolveSipError(task->status()).toLocal8Bit().constData()));

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
					PJ_LOG(4,(__FILE__,"Video preview started, cap_dev=%d",task->captureDev()));
				}
				else
				{
					PJ_LOG(1,(__FILE__,"Incompatible render device type: cap_dev=%d, ren_dev=%d, type=%d",task->captureDev(),task->renderDev(),task->windowType()));
				}
			}
			else
			{
				PJ_LOG(1,(__FILE__,"Failed to start video preview: cap_dev=%d, err=%d-%s",task->captureDev(),task->status(),resolveSipError(task->status()).toLocal8Bit().constData()));
			}
		}
		break;
	case SipTask::StopPreview:
		{
			SipTaskStopPreview *task = static_cast<SipTaskStopPreview *>(ATask);

			foreach(VideoWindow *widget, FVideoPreviewWidgets.values(task->captureDev()))
				widget->setSurface(NULL);

			if (task->status() == PJ_SUCCESS)
				PJ_LOG(4,(__FILE__,"Video preview stopped, cap_dev=%d",task->captureDev()));
			else
				PJ_LOG(1,(__FILE__,"Failed to stop video preview: cap_dev=%d, err=%d-%s",task->captureDev(),task->status(),resolveSipError(task->status()).toLocal8Bit().constData()));
		}
		break;
	default:
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
		PJ_LOG(4, (__FILE__, "NAT detected as %s", AResult->nat_type_name));
	else
		pjsua_perror(__FILE__, "NAT detection failed", AResult->status);
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
