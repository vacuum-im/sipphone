#ifndef SIPEVENT_H
#define SIPEVENT_H

#include <pjsua.h>

struct SipEvent 
{
	enum Type {
		Null,
		Error,
		RegState,
		IncomingCall,
		CallState,
		CallMediaState,
		CallMediaFormat
	};
	Type type;
};

struct SipEventError : 
	public SipEvent
{
	pj_status_t error;
};

struct SipEventRegState :
	public SipEvent
{
	pjsua_acc_id accIndex;
};

struct SipEventIncomingCall : 
	public SipEvent
{
	pjsua_acc_id accIndex;
	pjsua_call_id callIndex;
};

struct SipEventCallState :
	public SipEvent
{
	pjsip_inv_state state;
	pjsip_status_code status;
	quint32 duration;
	qint64 destroyWaitTime;
};

struct SipEventCallMediaState :
	public SipEvent
{
	pjsua_conf_port_id confSlot;
	pjsua_call_media_status mediaStatus;
};

struct SipEventCallMediaFormat :
	public SipEvent
{
	unsigned mediaIndex;
};

#endif // SIPEVENT_H
