#ifndef SIPWORKER_H
#define SIPWORKER_H

#include <QQueue>
#include <QMutex>
#include <QThread>
#include <QRunnable>
#include <QWaitCondition>
#include <interfaces/isipphone.h>
#include <pjsua.h>

class SipTask :
	public QRunnable
{
	friend class SipWorker;
public:
	enum Type {
		CreateStack,
		DestroyStack,
		StartPreview,
		StopPreview,
	};
public:
	SipTask(Type AType);
	virtual ~SipTask();
	Type type() const;
	QString taskId() const;
	pj_status_t status() const;
protected:
	Type FType;
	QString FTaskId;
	pj_status_t FStatus;
private:
	static quint32 FTaskCount;
};

class SipTaskCreateStack :
	public SipTask
{
public:
	struct Params {
		QString stun;
		bool enableIce;
		quint16 udpPort;
		quint16 tcpPort;
		QString userAgent;
		QString logFileName;
		pjsua_callback callBack;
		pjmedia_vid_dev_factory_create_func_ptr vdf;
	};
	SipTaskCreateStack(const Params &AParams);
protected:
	void run();
private:
	Params FParams;
};

class SipTaskDestroyStack :
	public SipTask
{
public:
	SipTaskDestroyStack();
protected:
	void run();
};

class SipTaskStartPreview :
	public SipTask
{
public:
	SipTaskStartPreview(int ACapDev, int ARendDev);
	int captureDev() const;
	int renderDev() const;
	void *window() const;
	int windowType() const;
protected:
	void run();
private:
	int FCapDev;
	int FRendDev;
	void *FWindow;
	int FWindowType;
};

class SipTaskStopPreview :
	public SipTask
{
public:
	SipTaskStopPreview(int ACapDev);
	int captureDev() const;
protected:
	void run();
private:
	int FCapDev;
};

class SipWorker : 
	public QThread
{
	Q_OBJECT;
public:
	SipWorker(QObject *AParent);
	~SipWorker();
	bool startTask(SipTask *ATask);
public slots:
	void quit();
signals:
	void taskFinished(SipTask *ATask);
protected:
	void run();
private:
	bool FQuit;
	QMutex FMutex;
	QWaitCondition FTaskReady;
	QQueue<SipTask *> FTasks;
};

#endif // SIPWORKER_H
