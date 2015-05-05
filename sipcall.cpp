#include "sipcall.h"

#include <QTimer>
#include <QMetaType>
#include <QDateTime>
#include <definitions/sipphone/statisticsparams.h>
#include <utils/logger.h>

#define CLOSE_MEDIA_DELAY  3000

SipCall::SipCall(const QUuid &AAccountId, pjsua_acc_id AAccIndex, const QString &ARemoteUri, QObject *AParent) : QObject(AParent)
{
	FAccIndex = AAccIndex;
	FCallIndex = PJSUA_INVALID_ID;
	FAccountId = AAccountId;
	FRemoteUri = ARemoteUri;

	FRole = ISipCall::Caller;
	FState = ISipCall::Inited;
	FStatusCode = ISipCall::SC_Undefined;

	initialize();
}

SipCall::SipCall(const QUuid &AAccountId, pjsua_acc_id AAccIndex, pjsua_call_id ACallIndex, QObject *AParent) : QObject(AParent)
{
	FAccIndex = AAccIndex;
	FCallIndex = ACallIndex;
	FAccountId = AAccountId;

	pjsua_call_info ci;
	pjsua_call_get_info(FCallIndex,&ci);
	FRemoteUri = QString::fromLocal8Bit(ci.remote_info.ptr,ci.remote_info.slen);

	FRole = ISipCall::Receiver;
	FState = ISipCall::Inited;
	FStatusCode = ci.last_status;

	initialize();
	printCallDump(true);
	setState(ISipCall::Ringing);
}

SipCall::~SipCall()
{
	destroyCall();

	foreach(VideoWindow *widget, FVideoPlaybackWidgets.values())
		delete widget;

	pjsua_conf_remove_port(FTonegenSlot);
	pjmedia_port_destroy(FTonegenPort);
	pj_pool_release(FTonegenPool);

	emit callDestroyed();
}

QUuid SipCall::accountId() const
{
	return FAccountId;
}

QString SipCall::remoteUri() const
{
	return FRemoteUri;
}

bool SipCall::isActive() const
{
	return FCallIndex>PJSUA_INVALID_ID ? pjsua_call_is_active(FCallIndex) : false;
}

ISipCall::Role SipCall::role() const
{
	return FRole;
}

ISipCall::State SipCall::state() const
{
	return FState;
}

quint32 SipCall::statusCode() const
{
	return FStatusCode;
}

QString SipCall::statusText() const
{
	return FStatusText;
}

quint32 SipCall::durationTime() const
{
	pjsua_call_info ci;
	if (FCallIndex>PJSUA_INVALID_ID && pjsua_call_get_info(FCallIndex,&ci)==PJ_SUCCESS)
		return ci.connect_duration.sec*1000 + ci.connect_duration.msec;
	return FTotalDurationTime;
}

bool SipCall::sendDtmf(const char *ADigits)
{
	if (FCallIndex > PJSUA_INVALID_ID)
	{
		pjmedia_tone_digit digits[16];
		pj_bzero(digits, sizeof(digits));

		int count = qMin(strlen(ADigits),PJ_ARRAY_SIZE(digits));
		for (int i=0; i<count; i++) 
		{
			digits[i].digit = ADigits[i];
			digits[i].on_msec = 120;
			digits[i].off_msec = 50;
			digits[i].volume = 0;
		}

		pj_status_t status = pjmedia_tonegen_play_digits(FTonegenPort,count,digits,0);
		if (status == PJ_SUCCESS)
		{
			LOG_DEBUG(QString("DTMF digits sent '%1', call=%2, uri=%3").arg(ADigits).arg(FCallIndex).arg(FRemoteUri));
			emit dtmfSent(ADigits);
			return true;
		}
		else
		{
			LOG_ERROR(QString("Failed to send DTMF digits '%1', call=%2, uri=%3: %4").arg(ADigits).arg(FCallIndex).arg(FRemoteUri).arg(resolveSipError(status)));
		}
	}
	else
	{
		LOG_ERROR(QString("Failed to send DTMF digits '%1', call=%2, uri=%3: Call is not active").arg(ADigits).arg(FCallIndex).arg(FRemoteUri));
	}

	return false;
}

bool SipCall::startCall(bool AWithVideo)
{
	if (FRole==Caller && FState==Inited)
	{
		pjsua_call_setting cs;
		pjsua_call_setting_default(&cs);
		cs.vid_cnt = AWithVideo ? 1 : 0;

		QByteArray uri8bit = FRemoteUri.toLocal8Bit();
		pj_str_t uri = pj_str(uri8bit.data());
		pj_status_t status = pjsua_call_make_call(FAccIndex,&uri,&cs,NULL,NULL,&FCallIndex);
		if (status == PJ_SUCCESS)
		{
			LOG_INFO(QString("Making outgoing SIP call, call=%1, uri=%2, video=%3").arg(FCallIndex).arg(FRemoteUri).arg(AWithVideo));
			return true;
		}
		else
		{
			LOG_ERROR(QString("Failed to make outgoing SIP call, call=%1, uri=%2, video=%3: %4").arg(FCallIndex).arg(FRemoteUri).arg(AWithVideo).arg(resolveSipError(status)));
			setError(status);
		}
	}
	else if (FRole==Receiver && FState==Ringing)
	{
		pjsua_call_setting cs;
		pjsua_call_setting_default(&cs);
		cs.vid_cnt = AWithVideo ? 1 : 0;

		pj_status_t status = pjsua_call_answer2(FCallIndex,&cs,PJSIP_SC_OK,NULL,NULL);
		if (status == PJ_SUCCESS)
		{
			LOG_INFO(QString("Accepting incoming SIP call, call=%1, uri=%2, video=%3").arg(FCallIndex).arg(FRemoteUri).arg(AWithVideo));
			return true;
		}
		else
		{
			LOG_ERROR(QString("Failed to accept incoming SIP call, call=%1, uri=%2, video=%3: %4").arg(FCallIndex).arg(FRemoteUri).arg(AWithVideo).arg(resolveSipError(status)));
			setError(status);
		}
	}
	else
	{
		REPORT_ERROR("Failed to start SIP call: Invalid call state");
	}
	return false;
}

bool SipCall::hangupCall(quint32 AStatusCode, const QString &AText)
{
	if (isActive())
	{
		QByteArray reason = AText.toLocal8Bit();
		pj_str_t pj_reason = pj_str(reason.data());
		pj_status_t status = pjsua_call_hangup(FCallIndex,AStatusCode,&pj_reason,NULL);
		if (status == PJ_SUCCESS)
		{
			LOG_INFO(QString("Hanging up SIP call, code=%1, call=%2, uri=%3").arg(AStatusCode).arg(FCallIndex).arg(FRemoteUri));
			return true;
		}
		else
		{
			LOG_ERROR(QString("Failed to hangup SIP call, code=%1, call=%2, uri=%3: %4").arg(AStatusCode).arg(FCallIndex).arg(FRemoteUri).arg(resolveSipError(status)));
			setError(status);
		}
	}
	return false;
}

bool SipCall::destroyCall(unsigned long AWaitForDisconnected)
{
	if (hangupCall(ISipCall::SC_Decline))
	{
		unsigned long waitTime = AWaitForDisconnected;
		QDateTime startTime = QDateTime::currentDateTime();
		
		while (isActive() && waitTime>0)
		{
			FDestroyLock.lock();
			FEventWait.wait(&FDestroyLock,qMin(waitTime,1000UL));
			FDestroyLock.unlock();

			unsigned long passedMsecs = startTime.msecsTo(QDateTime::currentDateTime());
			waitTime = AWaitForDisconnected>passedMsecs ? AWaitForDisconnected-passedMsecs : 0;
		}

		if (isActive())
		{
			LOG_DEBUG(QString("Waiting for SIP call disconnection, call=%1, uri=%2").arg(FCallIndex).arg(FRemoteUri));
			FDelayedDestroy = true;
			return false;
		}
	}

	LOG_INFO(QString("Destroying SIP call, call=%1, uri=%2").arg(FCallIndex).arg(FRemoteUri));
	int delayDestroy = qMax(FDestroyWaitTime-QDateTime::currentMSecsSinceEpoch(),(qint64)0);
	QTimer::singleShot(delayDestroy,this,SLOT(deleteLater()));

	return true;
}

bool SipCall::hasActiveMediaStream() const
{
	return FCallIndex!=PJSUA_INVALID_ID ? pjsua_call_has_media(FCallIndex) : false;
}

ISipMediaStream SipCall::findMediaStream(int AMediaIndex) const
{
	return AMediaIndex>=0 ? mediaStreams().value(AMediaIndex) : ISipMediaStream();
}

QList<ISipMediaStream> SipCall::mediaStreams(ISipMedia::Type AType) const
{
	QList<ISipMediaStream> streams;
	if (isActive())
	{
		pjsua_call_info ci;
		if (pjsua_call_get_info(FCallIndex,&ci) == PJ_SUCCESS)
		{
			for (unsigned index = 0; index<ci.media_cnt; index++)
			{
				ISipMediaStream stream;
				switch (ci.media[index].type)
				{
				case PJMEDIA_TYPE_AUDIO:
					stream.type = ISipMedia::Audio;
					break;
				case PJMEDIA_TYPE_VIDEO:
					stream.type = ISipMedia::Video;
					break;
				default:
					stream.type = ISipMedia::Unknown;
				}

				switch(ci.media[index].dir)
				{
				case PJMEDIA_DIR_CAPTURE:
					stream.dir = ISipMedia::Capture;
					break;
				case PJMEDIA_DIR_PLAYBACK:
					stream.dir = ISipMedia::Playback;
					break;
				case PJMEDIA_DIR_CAPTURE_PLAYBACK:
					stream.dir = ISipMedia::CaptureAndPlayback;
					break;
				default:
					stream.dir = ISipMedia::None;
				}
				
				if (AType==ISipMedia::Null || stream.type==AType)
				{
					stream.index = index;
					stream.state = (ISipMediaStream::State)ci.media[index].status;

					pjsua_stream_info si;
					if (ci.media[index].dir!=PJMEDIA_DIR_NONE && pjsua_call_get_stream_info(FCallIndex,index,&si)==PJ_SUCCESS)
					{
						if (si.type == PJMEDIA_TYPE_AUDIO)
						{
							stream.format.type = ISipMedia::Audio;
							stream.format.details.aud.clockRate = si.info.aud.param->info.clock_rate;
							stream.format.details.aud.channelCount = si.info.aud.param->info.channel_cnt;
							stream.format.details.aud.frameTimeUsec = si.info.aud.param->info.frm_ptime;
							stream.format.details.aud.bitsPerSample = si.info.aud.param->info.pcm_bits_per_sample;
							stream.format.details.aud.avgBitrate = si.info.aud.param->info.avg_bps;
							stream.format.details.aud.maxBitrate = si.info.aud.param->info.max_bps;
						}
						else if (si.type == PJMEDIA_TYPE_VIDEO)
						{
							if (si.info.vid.codec_param->dir == PJMEDIA_DIR_ENCODING)
							{
								stream.format.type = ISipMedia::Video;
								stream.format.details.vid.fpsNum = si.info.vid.codec_param->enc_fmt.det.vid.fps.num;
								stream.format.details.vid.fpsDenum = si.info.vid.codec_param->enc_fmt.det.vid.fps.denum;
								stream.format.details.vid.avgBitrate = si.info.vid.codec_param->enc_fmt.det.vid.avg_bps;
								stream.format.details.vid.maxBitrate = si.info.vid.codec_param->enc_fmt.det.vid.max_bps;
								stream.format.details.vid.width = si.info.vid.codec_param->enc_fmt.det.vid.size.w;
								stream.format.details.vid.height = si.info.vid.codec_param->enc_fmt.det.vid.size.h;
							}
							else if (si.info.vid.codec_param->dir == PJMEDIA_DIR_DECODING)
							{
								stream.format.type = ISipMedia::Video;
								stream.format.details.vid.fpsNum = si.info.vid.codec_param->dec_fmt.det.vid.fps.num;
								stream.format.details.vid.fpsDenum = si.info.vid.codec_param->dec_fmt.det.vid.fps.denum;
								stream.format.details.vid.avgBitrate = si.info.vid.codec_param->dec_fmt.det.vid.avg_bps;
								stream.format.details.vid.maxBitrate = si.info.vid.codec_param->dec_fmt.det.vid.max_bps;
								stream.format.details.vid.width = si.info.vid.codec_param->dec_fmt.det.vid.size.w;
								stream.format.details.vid.height = si.info.vid.codec_param->dec_fmt.det.vid.size.h;
							}
						}
					}

					streams.append(stream);
				}
			}
		}
	}
	return streams;
}

QVariant SipCall::mediaStreamProperty(int AMediaIndex, ISipMedia::Direction ADir, ISipMediaStream::Property AProperty) const
{
	pjsua_call_info ci;
	if (AMediaIndex>=0 && isActive() && pjsua_call_get_info(FCallIndex,&ci)==PJ_SUCCESS && AMediaIndex<(int)ci.media_cnt)
	{
		if (ci.media[AMediaIndex].type == PJMEDIA_TYPE_AUDIO)
		{
			switch (AProperty)
			{
			case ISipMediaStream::Enabled:
				{
					pjsua_conf_port_id sink = -1;
					pjsua_conf_port_id source =-1;
					if (ADir == ISipMedia::Capture)
					{
						source = 0;
						sink = ci.media[AMediaIndex].stream.aud.conf_slot;
					}
					else if (ADir == ISipMedia::Playback)
					{
						source = ci.media[AMediaIndex].stream.aud.conf_slot;
						sink = 0;
					}

					pjsua_conf_port_info pi;
					if (source>=0 && pjsua_conf_get_port_info(source,&pi)==PJ_SUCCESS)
					{
						for(unsigned int i=0; i<pi.listener_cnt; i++)
							if (pi.listeners[i] == sink)
								return QVariant(true);
						return QVariant(false);
					}
				}
				break;
			case ISipMediaStream::Volume:
				{
					if (ADir==ISipMedia::Capture || ADir==ISipMedia::Playback)
					{
						static const QVariant defVolume = 1.0;
						return FStreamProperties.value(AMediaIndex).value(ADir).value(AProperty,defVolume);
					}
				}
				break;
			default:
				break;
			}
		}
		else if (ci.media[AMediaIndex].type == PJMEDIA_TYPE_VIDEO)
		{
			switch (AProperty)
			{
			case ISipMediaStream::Enabled:
				{
					if (ADir == ISipMedia::Capture)
						return QVariant((ci.media[AMediaIndex].dir & PJMEDIA_DIR_CAPTURE) > 0);
					else if (ADir == ISipMedia::Playback)
						return QVariant((ci.media[AMediaIndex].dir & PJMEDIA_DIR_PLAYBACK) > 0);
				}
				break;
			case ISipMediaStream::Volume:
				{
					if (ADir==ISipMedia::Capture || ADir==ISipMedia::Playback)
					{
						static const QVariant defVolume = 1.0;
						return FStreamProperties.value(AMediaIndex).value(ADir).value(AProperty,defVolume);
					}
				}
				break;
			default:
				break;
			}
		}
	}
	return QVariant();
}

bool SipCall::setMediaStreamProperty(int AMediaIndex, ISipMedia::Direction ADir, ISipMediaStream::Property AProperty, const QVariant &AValue)
{
	pjsua_call_info ci;
	QVariant curValue = mediaStreamProperty(AMediaIndex,ADir,AProperty);
	if (curValue!=AValue && isActive() && pjsua_call_get_info(FCallIndex,&ci)==PJ_SUCCESS && AMediaIndex!=PJSUA_INVALID_ID && AMediaIndex<(int)ci.media_cnt)
	{
		pjmedia_dir pj_dir;
		switch(ADir)
		{
		case ISipMedia::Capture:
			pj_dir = PJMEDIA_DIR_CAPTURE;
			break;
		case ISipMedia::Playback:
			pj_dir = PJMEDIA_DIR_PLAYBACK;
			break;
		case ISipMedia::CaptureAndPlayback:
			pj_dir = PJMEDIA_DIR_CAPTURE_PLAYBACK;
			break;
		default:
			pj_dir = PJMEDIA_DIR_NONE;
		}

		if (ci.media[AMediaIndex].type == PJMEDIA_TYPE_AUDIO)
		{
			switch (AProperty)
			{
			case ISipMediaStream::Enabled:
				{
					pj_status_t status = PJ_SUCCESS;
					if ((ADir & ISipMedia::Capture) > 0)
					{
						if (AValue.toBool())
							status = pjsua_conf_connect(0,ci.media[AMediaIndex].stream.aud.conf_slot);
						else
							status = pjsua_conf_disconnect(0,ci.media[AMediaIndex].stream.aud.conf_slot);
					}
					if ((ADir & ISipMedia::Playback) > 0)
					{
						if (AValue.toBool())
							status = pjsua_conf_connect(ci.media[AMediaIndex].stream.aud.conf_slot,0);
						else
							status = pjsua_conf_disconnect(ci.media[AMediaIndex].stream.aud.conf_slot,0);
					}

					if (status == PJ_SUCCESS)
						LOG_DEBUG(QString("SIP media stream property changed, call=%1, media=%2, dir=%3, property=%4, value=%5").arg(FCallIndex).arg(AMediaIndex).arg(pj_dir).arg(AProperty).arg(AValue.toString()));
					else
						LOG_ERROR(QString("Failed to change SIP media stream property, call=%1, media=%2, dir=%3, property=%4, value=%5: %6").arg(FCallIndex).arg(AMediaIndex).arg(pj_dir).arg(AProperty).arg(AValue.toString()).arg(resolveSipError(status)));

					emit mediaChanged();
					return status == PJ_SUCCESS;
				}
			case ISipMediaStream::Volume:
				{
					pj_status_t status_cap = PJ_SUCCESS;
					if ((ADir & ISipMedia::Capture) > 0)
					{
						status_cap = pjsua_conf_adjust_tx_level(ci.media[AMediaIndex].stream.aud.conf_slot,AValue.toFloat());
						if (status_cap == PJ_SUCCESS)
							FStreamProperties[AMediaIndex][ISipMedia::Capture][ISipMediaStream::Volume] = AValue;
					}

					pj_status_t status_play = PJ_SUCCESS;
					if ((ADir & ISipMedia::Playback) > 0)
					{
						status_play = pjsua_conf_adjust_rx_level(ci.media[AMediaIndex].stream.aud.conf_slot,AValue.toFloat());
						if (status_play == PJ_SUCCESS)
							FStreamProperties[AMediaIndex][ISipMedia::Playback][ISipMediaStream::Volume] = AValue;
					}

					if (status_cap==PJ_SUCCESS && status_play==PJ_SUCCESS)
						LOG_DEBUG(QString("SIP media stream property changed, call=%1, media=%2, dir=%3, property=%4, value=%5").arg(FCallIndex).arg(AMediaIndex).arg(pj_dir).arg(AProperty).arg(AValue.toString()));
					else
						LOG_ERROR(QString("Failed to change SIP media stream property, call=%1, media=%2, dir=%3, property=%4, value=%5: %6").arg(FCallIndex).arg(AMediaIndex).arg(pj_dir).arg(AProperty).arg(AValue.toString()).arg(resolveSipError(status_play==PJ_SUCCESS ? status_cap : status_play)));

					emit mediaChanged();
					return status_cap==PJ_SUCCESS && status_play==PJ_SUCCESS;
				}
			default:
				LOG_ERROR(QString("Failed to change SIP media stream property, call=%1, media=%2, dir=%3, property=%4, value=%5: Unsupported audio property").arg(FCallIndex).arg(AMediaIndex).arg(pj_dir).arg(AProperty).arg(AValue.toString()));
				break;
			}
		}
		else if (ci.media[AMediaIndex].type == PJMEDIA_TYPE_VIDEO)
		{
			switch (AProperty)
			{
			case ISipMediaStream::Enabled:
				{
					pjsua_call_vid_strm_op_param param;
					pjsua_call_vid_strm_op_param_default(&param);

					param.med_idx = AMediaIndex;
					if (AValue.toBool())
						param.dir = (pjmedia_dir)(ci.media[AMediaIndex].dir | pj_dir);
					else
						param.dir = (pjmedia_dir)(ci.media[AMediaIndex].dir & ~pj_dir);

					pj_status_t status = pjsua_call_set_vid_strm(FCallIndex,PJSUA_CALL_VID_STRM_CHANGE_DIR,&param);

					if (status == PJ_SUCCESS)
						LOG_DEBUG(QString("SIP media stream property changed, call=%1, media=%2, dir=%3, property=%4, value=%5").arg(FCallIndex).arg(AMediaIndex).arg(pj_dir).arg(AProperty).arg(AValue.toString()));
					else
						LOG_ERROR(QString("Failed to change SIP media stream property, call=%1, media=%2, dir=%3, property=%4, value=%5: %6").arg(FCallIndex).arg(AMediaIndex).arg(pj_dir).arg(AProperty).arg(AValue.toString()).arg(resolveSipError(status)));

					emit mediaChanged();
					return status == PJ_SUCCESS;
				}
			case ISipMediaStream::Volume:
				{
					pjsua_call_vid_strm_op_param param;
					pjsua_call_vid_strm_op_param_default(&param);

					param.med_idx = AMediaIndex;
					param.dir = pj_dir;

					pj_status_t status = -1;
					if (AValue.toFloat() < 0.1)
						status = pjsua_call_set_vid_strm(FCallIndex,PJSUA_CALL_VID_STRM_STOP_TRANSMIT,&param);
					else if (AValue.toFloat() > 0.9)
						status = pjsua_call_set_vid_strm(FCallIndex,PJSUA_CALL_VID_STRM_START_TRANSMIT,&param);

					if (status == PJ_SUCCESS)
					{
						if ((ADir & ISipMedia::Capture) > 0)
							FStreamProperties[AMediaIndex][ISipMedia::Capture][ISipMediaStream::Volume] = AValue;
						if ((ADir & ISipMedia::Playback) > 0)
							FStreamProperties[AMediaIndex][ISipMedia::Playback][ISipMediaStream::Volume] = AValue;
						LOG_DEBUG(QString("SIP media stream property changed, call=%1, media=%2, dir=%3, property=%4, value=%5").arg(FCallIndex).arg(AMediaIndex).arg(pj_dir).arg(AProperty).arg(AValue.toString()));
					}
					else
					{
						LOG_ERROR(QString("Failed to change SIP media stream property, call=%1, media=%2, dir=%3, property=%4, value=%5: %6").arg(FCallIndex).arg(AMediaIndex).arg(pj_dir).arg(AProperty).arg(AValue.toString()).arg(resolveSipError(status)));
					}

					emit mediaChanged();
					return status == PJ_SUCCESS;
				}
			default:
				LOG_ERROR(QString("Failed to change SIP media stream property, call=%1, media=%2, dir=%3, property=%4, value=%5: Unsupported video property").arg(FCallIndex).arg(AMediaIndex).arg(pj_dir).arg(AProperty).arg(AValue.toString()));
				break;
			}
		}
		else
		{
			LOG_ERROR(QString("Failed to change SIP media stream property, call=%1, media=%2, dir=%3, property=%4, value=%5: Invalid media type").arg(FCallIndex).arg(AMediaIndex).arg(pj_dir).arg(AProperty).arg(AValue.toString()));
		}
	}
	else if (curValue!=AValue && isActive())
	{
		LOG_ERROR(QString("Failed to change SIP media stream property, call=%1, media=%2, property=%3, value=%4: Invalid media index").arg(FCallIndex).arg(AMediaIndex).arg(AProperty).arg(AValue.toString()));
	}
	return false;
}

QWidget *SipCall::getVideoPlaybackWidget(int AMediaIndex, QWidget *AParent)
{
	VideoWindow *widget = NULL;
	ISipMediaStream stream = findMediaStream(AMediaIndex);
	if (stream.index==AMediaIndex && stream.type==ISipMedia::Video && (stream.dir & ISipMedia::Playback)>0)
	{
		pjsua_call_info ci;
		if (FCallIndex>=0 && pjsua_call_get_info(FCallIndex,&ci)==PJ_SUCCESS && AMediaIndex<(int)ci.media_cnt && ci.media[AMediaIndex].stream.vid.win_in!=PJSUA_INVALID_ID)
		{
			pjsua_vid_win_info wi;
			if (pjsua_vid_win_get_info(ci.media[AMediaIndex].stream.vid.win_in,&wi) == PJ_SUCCESS)
			{
				if (wi.hwnd.type == QT_RENDER_VID_DEV_TYPE)
				{
					widget = new VideoWindow(AParent);
					widget->setSurface((VideoSurface *)wi.hwnd.info.window);
					connect(widget,SIGNAL(windowDestroyed()),SLOT(onVideoPlaybackWidgetDestroyed()));
					FVideoPlaybackWidgets.insertMulti(AMediaIndex,widget);
					LOG_DEBUG(QString("SIP video playback widget created: call=%1, media=%2").arg(FCallIndex).arg(AMediaIndex));
				}
				else
				{
					LOG_ERROR(QString("Failed to create SIP video playback widget, call=%1, media=%2: Incompatible render device type (%3)").arg(FCallIndex).arg(AMediaIndex).arg(wi.hwnd.type));
				}
			}
			else
			{
				LOG_ERROR(QString("Failed to create SIP video playback widget, call=%1, media=%2: Media stream window info not found").arg(FCallIndex).arg(AMediaIndex));
			}
		}
		else
		{
			LOG_ERROR(QString("Failed to create SIP video playback widget, call=%1, media=%2: Invalid media index").arg(FCallIndex).arg(AMediaIndex));
		}
	}
	else if (stream.index!=AMediaIndex)
	{
		LOG_ERROR(QString("Failed to create SIP video playback widget, call=%1, media=%2: Media stream not found").arg(FCallIndex).arg(AMediaIndex));
	}
	else
	{
		LOG_ERROR(QString("Failed to create SIP video playback widget, call=%1, media=%2: Invalid media stream").arg(FCallIndex).arg(AMediaIndex));
	}
	return widget;
}

void SipCall::initialize()
{
	FDestroyWaitTime = 0;
	FDelayedDestroy = false;
	FTotalDurationTime = 0;

	FTonegenPool = pjsua_pool_create("tonegen-pool", 512, 512);
	pjmedia_tonegen_create(FTonegenPool, 8000, 1, 160, 16, 0, &FTonegenPort);
	pjsua_conf_add_port(FTonegenPool, FTonegenPort, &FTonegenSlot);
	pjsua_conf_connect(FTonegenSlot,0);
}

void SipCall::setState(State AState)
{
	if (FState < AState)
	{
		LOG_DEBUG(QString("Call state changed, state=%1, call=%2, uri=%3").arg(AState).arg(FCallIndex).arg(FRemoteUri));

		FState = AState;

		switch (AState)
		{
		case ISipCall::Connecting:
			Logger::startTiming(STMP_SIPPHONE_CALL_NEGOTIATION,FRemoteUri);
			break;
		case ISipCall::Confirmed:
			REPORT_TIMING(STMP_SIPPHONE_CALL_NEGOTIATION,Logger::finishTiming(STMP_SIPPHONE_CALL_NEGOTIATION,FRemoteUri));
			Logger::startTiming(STMP_SIPPHONE_CALL_DURATION,FRemoteUri);
			REPORT_EVENT(SEVP_SIPPHONE_CALL_SUCCESS,1);
			break;
		case Disconnected:
			REPORT_TIMING(STMP_SIPPHONE_CALL_DURATION,Logger::finishTiming(STMP_SIPPHONE_CALL_DURATION,FRemoteUri));
			Logger::finishTiming(STMP_SIPPHONE_CALL_NEGOTIATION,FRemoteUri);
			break;
		case Aborted:
			Logger::finishTiming(STMP_SIPPHONE_CALL_NEGOTIATION,FRemoteUri);
			Logger::finishTiming(STMP_SIPPHONE_CALL_DURATION,FRemoteUri);
			REPORT_EVENT(SEVP_SIPPHONE_CALL_FAILURE,1);
			break;
		default:
			break;
		}

		if (FState==Disconnected || FState==Aborted)
		{
			FCallIndex = PJSUA_INVALID_ID;
			if (FDelayedDestroy)
				destroyCall(0);
		}

		emit stateChanged();
	}
}

void SipCall::setError(pj_status_t AStatus)
{
	if (FState != Aborted)
	{
		setStatus(PJSIP_SC_INTERNAL_SERVER_ERROR,resolveSipError(AStatus));
		setState(Aborted);
	}
}

bool SipCall::isErrorStatus(pjsip_status_code ACode)
{
	switch (ACode)
	{
	case PJSIP_SC_TRYING:
	case PJSIP_SC_RINGING:
	case PJSIP_SC_CALL_BEING_FORWARDED:
	case PJSIP_SC_QUEUED:
	case PJSIP_SC_PROGRESS:
	case PJSIP_SC_OK:
	case PJSIP_SC_ACCEPTED:
	case PJSIP_SC_BUSY_HERE:
	case PJSIP_SC_BUSY_EVERYWHERE:
	case PJSIP_SC_DECLINE:
		return false;
	default:
		return true;
	}
}

void SipCall::setStatus(quint32 ACode, const QString &AText)
{
	if (FStatusCode != ACode)
	{
		FStatusCode = ACode;
		FStatusText = AText;
		LOG_DEBUG(QString("Call status changed, code=%1, message=%2, call=%3, uri=%4").arg(FStatusCode).arg(FStatusText).arg(FCallIndex).arg(FRemoteUri));
		emit statusChanged();
	}
}

QString SipCall::resolveSipError(int ACode) const
{
	char errmsg[PJ_ERR_MSG_SIZE];
	pj_strerror(ACode, errmsg, sizeof(errmsg));
	return QString(errmsg);
}

void SipCall::printCallDump(bool AWithMedia) const
{
	char buf[PJ_LOG_MAX_SIZE];
	if (FCallIndex>=0 && pjsua_call_dump(FCallIndex,(AWithMedia ? PJ_TRUE : PJ_FALSE),buf,sizeof(buf)," ") == PJ_SUCCESS)
	{
		pjsua_call_info ci;
		if (pjsua_call_get_info(FCallIndex,&ci) == PJ_SUCCESS)
		{
			unsigned pos = strlen(buf);
			unsigned end= sizeof(buf);

			buf[end--]='\0';
			pos += pj_ansi_snprintf(buf+pos,end-pos," Call Info:\n");

			pos += pj_ansi_snprintf(buf+pos,end-pos,
				"   local_info=%s, local_contact=%s\n"
				"   remote_info=%s, remote_contact=%s\n"
				"   flags=%d, aud_cnt=%d, vid_cnt=%d, rem_aud=%d, rem_vid=%d\n"
				"   media_status=%d, media_dir=%d, conf_slot=%d\n",
				ci.local_info.ptr, ci.local_contact.ptr,
				ci.remote_info.ptr, ci.remote_contact.ptr,
				ci.setting.flag, ci.setting.aud_cnt, ci.setting.vid_cnt, ci.rem_aud_cnt, ci.rem_vid_cnt,
				ci.media_status, ci.media_dir, ci.conf_slot);

			if (AWithMedia)
			{
				pos += pj_ansi_snprintf(buf+pos,end-pos,"   media_cnt=%d\n",ci.media_cnt);
				for (unsigned i=0; i<ci.media_cnt; i++)
				{
					pjsua_call_media_info *mi = &ci.media[i];

					pos += pj_ansi_snprintf(buf+pos,end-pos,
						"     #%d type=%d, dir=%d, status=%d\n",
						mi->index, mi->type, mi->dir, mi->status);
				}

				pos += pj_ansi_snprintf(buf+pos,end-pos,"   prov_media_cnt=%d\n",ci.prov_media_cnt);
				for (unsigned i=0; i<ci.prov_media_cnt; i++)
				{
					pjsua_call_media_info *mi = &ci.prov_media[i];

					pos += pj_ansi_snprintf(buf+pos,end-pos,
						"     #%d type=%d, dir=%d, status=%d\n",
						mi->index, mi->type, mi->dir, mi->status);
				}
			}
		}

		LOG_DEBUG(QString("Call dump, call=%1\n%2").arg(FCallIndex).arg(buf));
	}
}

void SipCall::updateVideoPlaybackWidgets(const QList<int> &AMediaIndexes)
{
	if (!FVideoPlaybackWidgets.isEmpty())
	{
		pjsua_call_info ci;
		pj_status_t status = FCallIndex!=PJSUA_INVALID_ID ? pjsua_call_get_info(FCallIndex,&ci) : -1;
		foreach(int mediaIndex, AMediaIndexes)
		{
			foreach(VideoWindow *widget, FVideoPlaybackWidgets.values(mediaIndex))
			{
				if (status==PJ_SUCCESS && mediaIndex<(int)ci.media_cnt && ci.media[mediaIndex].stream.vid.win_in!=PJSUA_INVALID_ID)
				{
					pjsua_vid_win_info wi;
					if (pjsua_vid_win_get_info(ci.media[mediaIndex].stream.vid.win_in,&wi)==PJ_SUCCESS && wi.hwnd.type==QT_RENDER_VID_DEV_TYPE)
						widget->setSurface((VideoSurface *)wi.hwnd.info.window);
					else
						widget->setSurface(NULL);
				}
				else
				{
					widget->setSurface(NULL);
				}
			}
		}
	}
}

void SipCall::processSipEvent(SipEvent *AEvent)
{
	switch (AEvent->type)
	{
	case SipEvent::CallState:
		{
			SipEventCallState *se = static_cast<SipEventCallState *>(AEvent);

			FTotalDurationTime = se->duration;
			FDestroyWaitTime = se->destroyWaitTime;
			setStatus(se->status,pjsip_get_status_text(se->status)->ptr);

			switch (se->state)
			{
			case PJSIP_INV_STATE_NULL:
				setState(Inited);
				break;
			case PJSIP_INV_STATE_CALLING:
			case PJSIP_INV_STATE_INCOMING:
				setState(Calling);
				break;
			case PJSIP_INV_STATE_EARLY:
				setState(Ringing);
				break;
			case PJSIP_INV_STATE_CONNECTING:
				setState(Connecting);
				break;
			case PJSIP_INV_STATE_CONFIRMED:
				setState(Confirmed);
				break;
			case PJSIP_INV_STATE_DISCONNECTED:
				setState(isErrorStatus(se->status) ? Aborted : Disconnected);
				break;
			default:
				break;
			};
			delete se;
		}
		break;
	case SipEvent::CallMediaState:
		{
			SipEventCallMediaState *se = static_cast<SipEventCallMediaState *>(AEvent);
			switch (se->mediaStatus)
			{
			case PJSUA_CALL_MEDIA_ACTIVE:
				{
					pjsua_conf_port_id conf = 0;
					pjsua_conf_connect(se->confSlot,conf);
					pjsua_conf_connect(conf,se->confSlot);
					pjsua_conf_connect(FTonegenSlot,se->confSlot);
				}
				break;
			case PJSUA_CALL_MEDIA_ERROR:
				{
					if (isActive())
					{
						pj_str_t reason = pj_str((char *)"ICE negotiation failed");
						pjsua_call_hangup(FCallIndex,PJSIP_SC_INTERNAL_SERVER_ERROR,&reason,NULL);
					}
				}
				break;
			default:
				break;
			}
			delete se;

			updateVideoPlaybackWidgets(FVideoPlaybackWidgets.keys());
			emit mediaChanged();
		}
		break;
	case SipEvent::Error:
		{
			SipEventError *se = static_cast<SipEventError *>(AEvent);
			setError(se->error);
			delete se;
		}
		break;
	default:
		REPORT_ERROR(QString("Received unexpected SIP event, call=%1, uri=%2, type=%3").arg(FCallIndex).arg(FRemoteUri).arg(AEvent->type));
		delete AEvent;
	}
}

void SipCall::onVideoPlaybackWidgetDestroyed()
{
	VideoWindow *widget = qobject_cast<VideoWindow *>(sender());
	if (widget)
	{
		int mediaIndex = FVideoPlaybackWidgets.key(widget);
		FVideoPlaybackWidgets.remove(mediaIndex,widget);
		LOG_DEBUG(QString("Video playback widget destroyed: call=%1, uri=%2, media=%3").arg(FCallIndex).arg(FRemoteUri).arg(mediaIndex));
	}
}

void SipCall::pjcbOnCallState()
{
	pjsua_call_info ci;
	pj_status_t status = pjsua_call_get_info(FCallIndex,&ci);

	printCallDump(true);

	if (status == PJ_SUCCESS)
	{
		SipEventCallState *se = new SipEventCallState;
		se->type = SipEvent::CallState;
		se->state = ci.state;
		se->status = ci.last_status;
		se->duration = ci.connect_duration.sec*1000 + ci.connect_duration.msec;
		se->destroyWaitTime = ci.media_status!=PJSUA_CALL_MEDIA_NONE ? QDateTime::currentMSecsSinceEpoch()+CLOSE_MEDIA_DELAY : 0;
		QMetaObject::invokeMethod(this,"processSipEvent",Qt::QueuedConnection,Q_ARG(SipEvent *,se));
	}
	else
	{
		SipEventError *se = new SipEventError;
		se->type = SipEvent::Error;
		se->error = status;
		QMetaObject::invokeMethod(this,"processSipEvent",Qt::QueuedConnection,Q_ARG(SipEvent *,se));
	}
	
	FEventWait.wakeAll();
}

void SipCall::pjcbOnCallMediaState()
{
	pjsua_call_info ci;
	pj_status_t status = pjsua_call_get_info(FCallIndex, &ci);

	printCallDump(true);

	if (status == PJ_SUCCESS)
	{
		SipEventCallMediaState *se = new SipEventCallMediaState;
		se->type = SipEvent::CallMediaState;
		se->confSlot = ci.conf_slot;
		se->mediaStatus = ci.media_status;
		QMetaObject::invokeMethod(this,"processSipEvent",Qt::QueuedConnection,Q_ARG(SipEvent *,se));
	}
	else
	{
		SipEventError *se = new SipEventError;
		se->type = SipEvent::Error;
		se->error = status;
		QMetaObject::invokeMethod(this,"processSipEvent",Qt::QueuedConnection,Q_ARG(SipEvent *,se));
	}

	FEventWait.wakeAll();
}

void SipCall::pjcbOnCallMediaEvent(unsigned AMediaIndex, pjmedia_event *AEvent)
{
	Q_UNUSED(AMediaIndex); Q_UNUSED(AEvent);
}
