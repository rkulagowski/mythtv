#include <QAtomicInt>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>
#include <QList>
#include <QQueue>
#include <QHash>
#include <QCoreApplication>
#include <QFileInfo>
#include <QStringList>
#include <QMap>
#include <QRegExp>
#include <QTimer>
#include <QSocketNotifier>
#include <iostream>

using namespace std;

#include "mythlogging.h"
#include "logging.h"
#include "loggingserver.h"
#include "mythconfig.h"
#include "mythdb.h"
#include "mythcorecontext.h"
#include "dbutil.h"
#include "exitcodes.h"
#include "compat.h"

#include <stdlib.h>
#ifndef _WIN32
#include <syslog.h>
#endif
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#if HAVE_GETTIMEOFDAY
#include <sys/time.h>
#endif
#include <signal.h>

// Various ways to get to thread's tid
#if defined(linux)
#include <sys/syscall.h>
#elif defined(__FreeBSD__)
extern "C" {
#include <sys/ucontext.h>
#include <sys/thr.h>
}
#elif CONFIG_DARWIN
#include <mach/mach.h>
#endif

static QMutex                      loggerMapMutex;
static QMap<QString, LoggerBase *> loggerMap;

LogServerThread                    *logServerThread = NULL;
LogForwardThread                   *logForwardThread = NULL;

static QMutex                       logThreadStartedMutex;
static QWaitCondition               logThreadStarted;
static bool                         logThreadFinished = false;

typedef QList<LoggerBase *> LoggerList;

typedef struct {
    LoggerList *list;
    qlonglong   epoch;
} LoggerListItem;
typedef QMap<QString, LoggerListItem *> ClientMap;

typedef QList<QString> ClientList;
typedef QMap<LoggerBase *, ClientList *> RevClientMap;

static QMutex                       logClientMapMutex;
static ClientMap                    logClientMap;

static QAtomicInt                   logClientCount;

static QMutex                       logRevClientMapMutex;
static RevClientMap                 logRevClientMap;

static QMutex                       logMsgListMutex;
static LogMessageList               logMsgList;
static QWaitCondition               logMsgListNotEmpty;

static ClientList                   logClientToDel;
static QMutex                       logClientToDelMutex;

static QAtomicInt                   msgsSinceHeartbeat;

#define TIMESTAMP_MAX 30
#define MAX_STRING_LENGTH (LOGLINE_MAX+120)

#ifndef _WIN32
static int sighupFd[2];
void logSighup(int signum);
#endif


/// \brief LoggerBase class constructor.  Adds the new logger instance to the
///        loggerMap.
/// \param string a C-string of the handle for this instance (NULL if unused)
LoggerBase::LoggerBase(const char *string)
{
    QMutexLocker locker(&loggerMapMutex);
    if (string)
    {
        m_handle = strdup(string);
        loggerMap.insert(QString(m_handle), this);
    }
    else
    {
        m_handle = NULL;
        loggerMap.insert(QString(""), this);
    }
}


/// \brief LoggerBase deconstructor.  Removes the logger instance from the
///        loggerMap.
LoggerBase::~LoggerBase()
{
    QMutexLocker locker(&loggerMapMutex);
    loggerMap.remove(QString(m_handle));

    if (m_handle)
        free(m_handle);
}


/// \brief FileLogger constructor
/// \param filename Filename of the logfile.
FileLogger::FileLogger(const char *filename) :
        LoggerBase(filename), m_opened(false), m_fd(-1), m_zmqSock(NULL)
{
    m_fd = open(filename, O_WRONLY|O_CREAT|O_APPEND, 0664);
    m_opened = (m_fd != -1);
    LOG(VB_GENERAL, LOG_INFO, QString("Added logging to %1")
             .arg(filename));
}


/// \brief FileLogger deconstructor - close the logfile
FileLogger::~FileLogger()
{
    if( m_opened )
    {
        LOG(VB_GENERAL, LOG_INFO, QString("Removed logging to %1")
            .arg(m_handle));
        close(m_fd);
        m_fd = -1;
        m_opened = false;
    }

    m_zmqSock->unsubscribeFrom(QByteArray(""));
    m_zmqSock->setLinger(0);
    m_zmqSock->disconnect(this);
    m_zmqSock->close();
    m_zmqSock->deleteLater();
}

/// \brief Reopen the logfile after a SIGHUP.  Log files only (no console).
///        This allows for logrollers to be used.
void FileLogger::reopen(void)
{
    close(m_fd);

    m_fd = open(m_handle, O_WRONLY|O_CREAT|O_APPEND, 0664);
    m_opened = (m_fd != -1);
    LOG(VB_GENERAL, LOG_INFO, QString("Rolled logging on %1") .arg(m_handle));
}

/// \brief Process a log message, writing to the logfile
/// \param item LoggingItem containing the log message to process
bool FileLogger::logmsg(LoggingItem *item)
{
    char                line[MAX_STRING_LENGTH];
    char                usPart[9];
    char                timestamp[TIMESTAMP_MAX];

    if (!m_opened)
        return false;

    item->refcount.ref();

    time_t epoch = item->epoch();
    struct tm tm;
    localtime_r(&epoch, &tm);

    strftime(timestamp, TIMESTAMP_MAX-8, "%Y-%m-%d %H:%M:%S",
             (const struct tm *)&tm);
    snprintf( usPart, 9, ".%06d", (int)(item->usec()) );
    strcat( timestamp, usPart );

    char shortname;
    {
        QMutexLocker locker(&loglevelMapMutex);
        LoglevelMap::iterator it = loglevelMap.find(item->level());
        if (it == loglevelMap.end())
            shortname = '-';
        else
            shortname = (*it)->shortname;
    }

    if( item->tid() )
        snprintf( line, MAX_STRING_LENGTH, 
                  "%s %c [%d/%" PREFIX64 "d] %s %s:%d (%s) - %s\n",
                  timestamp, shortname, item->pid(), item->tid(),
                  item->rawThreadName(), item->rawFile(), item->line(),
                  item->rawFunction(), item->rawMessage() );
    else
        snprintf( line, MAX_STRING_LENGTH,
                  "%s %c [%d] %s %s:%d (%s) - %s\n",
                  timestamp, shortname, item->pid(), item->rawThreadName(),
                  item->rawFile(), item->line(), item->rawFunction(),
                  item->rawMessage() );

    int result = write(m_fd, line, strlen(line));

    if( result == -1 )
    {
        LOG(VB_GENERAL, LOG_ERR,
                 QString("Closed Log output on fd %1 due to errors").arg(m_fd));
        m_opened = false;
        close( m_fd );
        return false;
    }
    return true;
}

void FileLogger::setupZMQSocket(void)
{
    nzmqt::ZMQContext *ctx = logForwardThread->getZMQContext();
    m_zmqSock = ctx->createSocket(nzmqt::ZMQSocket::TYP_SUB, this);
    connect(m_zmqSock, SIGNAL(messageReceived(const QList<QByteArray>&)),
            this, SLOT(receivedMessage(const QList<QByteArray>&)),
            Qt::QueuedConnection);
    m_zmqSock->subscribeTo(QByteArray(""));
    m_zmqSock->connectTo("inproc://loggers");
}


#ifndef _WIN32
/// \brief SyslogLogger constructor
/// \param facility Syslog facility to use in logging
SyslogLogger::SyslogLogger() : 
    LoggerBase(NULL), m_opened(false), m_zmqSock(NULL)
{
    openlog(NULL, LOG_NDELAY, 0 );
    m_opened = true;

    LOG(VB_GENERAL, LOG_INFO, "Added syslogging");
}

/// \brief SyslogLogger deconstructor.
SyslogLogger::~SyslogLogger()
{
    LOG(VB_GENERAL, LOG_INFO, "Removing syslogging");
    closelog();

    m_zmqSock->unsubscribeFrom(QByteArray(""));
    m_zmqSock->setLinger(0);
    m_zmqSock->disconnect(this);
    m_zmqSock->close();
    m_zmqSock->deleteLater();
}


/// \brief Process a log message, logging to syslog
/// \param item LoggingItem containing the log message to process
bool SyslogLogger::logmsg(LoggingItem *item)
{
    if (!m_opened || item->facility() <= 0)
        return false;

    char shortname;

    {
        QMutexLocker locker(&loglevelMapMutex);
        LoglevelDef *lev = loglevelMap.value(item->level(), NULL);
        if (!lev)
            shortname = '-';
        else
            shortname = lev->shortname;
    }
    syslog(item->level() | item->facility(), "%s[%d]: %c %s %s:%d (%s) %s",
           item->rawAppName(), item->pid(), shortname, item->rawThreadName(),
           item->rawFile(), item->line(), item->rawFunction(),
           item->rawMessage());

    return true;
}

void SyslogLogger::setupZMQSocket(void)
{
    nzmqt::ZMQContext *ctx = logForwardThread->getZMQContext();
    m_zmqSock = ctx->createSocket(nzmqt::ZMQSocket::TYP_SUB, this);
    connect(m_zmqSock, SIGNAL(messageReceived(const QList<QByteArray>&)),
            this, SLOT(receivedMessage(const QList<QByteArray>&)),
            Qt::QueuedConnection);
    m_zmqSock->subscribeTo(QByteArray(""));
    m_zmqSock->connectTo("inproc://loggers");
}
#else

// Windows doesn't have syslog support

SyslogLogger::SyslogLogger() :
    LoggerBase(NULL), m_opened(false), m_zmqSock(NULL)
{
}

SyslogLogger::~SyslogLogger()
{
}

bool SyslogLogger::logmsg(LoggingItem *item)
{
    (void)item;
    return false;
}

void SyslogLogger::setupZMQSocket(void)
{
}

#endif

const int DatabaseLogger::kMinDisabledTime = 1000;

/// \brief DatabaseLogger constructor
/// \param table C-string of the database table to log to
DatabaseLogger::DatabaseLogger(const char *table) :
    LoggerBase(table), m_opened(false), m_loggingTableExists(false),
    m_zmqSock(NULL)
{
    m_query = QString(
        "INSERT INTO %1 "
        "    (host, application, pid, tid, thread, filename, "
        "     line, function, msgtime, level, message) "
        "VALUES (:HOST, :APP, :PID, :TID, :THREAD, :FILENAME, "
        "        :LINE, :FUNCTION, :MSGTIME, :LEVEL, :MESSAGE)")
        .arg(m_handle);

    LOG(VB_GENERAL, LOG_INFO, QString("Added database logging to table %1")
        .arg(m_handle));

    m_thread = new DBLoggerThread(this);
    m_thread->start();

    m_opened = true;
    m_disabled = false;
}

/// \brief DatabaseLogger deconstructor
DatabaseLogger::~DatabaseLogger()
{
    LOG(VB_GENERAL, LOG_INFO, "Removing database logging");

    stopDatabaseAccess();

    m_zmqSock->unsubscribeFrom(QByteArray(""));
    m_zmqSock->setLinger(0);
    m_zmqSock->disconnect(this);
    m_zmqSock->close();
    m_zmqSock->deleteLater();
}

/// \brief Stop logging to the database and wait for the thread to stop.
void DatabaseLogger::stopDatabaseAccess(void)
{
    if( m_thread )
    {
        m_thread->stop();
        m_thread->wait();
        delete m_thread;
        m_thread = NULL;
    }
}

/// \brief Process a log message, queuing it for logging to the database
/// \param item LoggingItem containing the log message to process
bool DatabaseLogger::logmsg(LoggingItem *item)
{
    if (!m_thread)
        return false;

    if (!m_thread->isRunning())
    {
        m_disabled = true;
        m_disabledTime.start();
    }

    if (!m_disabled && m_thread->queueFull())
    {
        m_disabled = true;
        m_disabledTime.start();
        LOG(VB_GENERAL, LOG_CRIT,
            "Disabling DB Logging: too many messages queued");
        return false;
    }

    if (m_disabled && m_disabledTime.elapsed() > kMinDisabledTime)
    {
        if (isDatabaseReady() && !m_thread->queueFull())
        {
            m_disabled = false;
            LOG(VB_GENERAL, LOG_CRIT, "Reenabling DB Logging");
        }
    }

    if (m_disabled)
        return false;

    item->refcount.ref();
    m_thread->enqueue(item);
    return true;
}

void DatabaseLogger::setupZMQSocket(void)
{
    nzmqt::ZMQContext *ctx = logForwardThread->getZMQContext();
    m_zmqSock = ctx->createSocket(nzmqt::ZMQSocket::TYP_SUB, this);
    connect(m_zmqSock, SIGNAL(messageReceived(const QList<QByteArray>&)),
            this, SLOT(receivedMessage(const QList<QByteArray>&)),
            Qt::QueuedConnection);
    m_zmqSock->subscribeTo(QByteArray(""));
    m_zmqSock->connectTo("inproc://loggers");
}


/// \brief Actually insert a log message from the queue into the database
/// \param query    The database insert query to use
/// \param item     LoggingItem containing the log message to insert
bool DatabaseLogger::logqmsg(MSqlQuery &query, LoggingItem *item)
{
    char        timestamp[TIMESTAMP_MAX];

    time_t epoch = item->epoch();
    struct tm tm;
    localtime_r(&epoch, &tm);

    strftime(timestamp, TIMESTAMP_MAX-8, "%Y-%m-%d %H:%M:%S",
             (const struct tm *)&tm);

    query.bindValue(":TID",         item->tid());
    query.bindValue(":THREAD",      item->threadName());
    query.bindValue(":FILENAME",    item->file());
    query.bindValue(":LINE",        item->line());
    query.bindValue(":FUNCTION",    item->function());
    query.bindValue(":MSGTIME",     timestamp);
    query.bindValue(":LEVEL",       item->level());
    query.bindValue(":MESSAGE",     item->message());
    query.bindValue(":APP",         item->appName());
    query.bindValue(":PID",         item->pid());

    if (!query.exec())
    {
        // Suppress Driver not loaded errors that occur at startup.
        // and suppress additional errors for one second after the
        // previous error (to avoid spamming the log).
        QSqlError err = query.lastError();
        if ((err.type() != 1 || err.number() != -1) &&
            (!m_errorLoggingTime.isValid() ||
             (m_errorLoggingTime.elapsed() > 1000)))
        {
            MythDB::DBError("DBLogging", query);
            m_errorLoggingTime.start();
        }
        return false;
    }

    item->deleteItem();

    return true;
}

/// \brief Prepare the database query for use, and bind constant values to it.
/// \param query    The database query to prepare
void DatabaseLogger::prepare(MSqlQuery &query)
{
    query.prepare(m_query);
    query.bindValue(":HOST", gCoreContext->GetHostName());
}

/// \brief Check if the database is ready for use
/// \return true when database is ready, false otherwise
bool DatabaseLogger::isDatabaseReady(void)
{
    bool ready = false;
    MythDB *db = GetMythDB();

    if ((db) && db->HaveValidDatabase())
    {
        if ( !m_loggingTableExists )
            m_loggingTableExists = tableExists(m_handle);

        if ( m_loggingTableExists )
            ready = true;
    }

    return ready;
}

/// \brief Checks whether table exists and is ready for writing
/// \param  table  The name of the table to check (without schema name)
/// \return true if table exists in schema or false if not
bool DatabaseLogger::tableExists(const QString &table)
{
    bool result = false;
    MSqlQuery query(MSqlQuery::InitCon());
    if (query.isConnected())
    {
        QString sql = "SELECT INFORMATION_SCHEMA.COLUMNS.COLUMN_NAME "
                      "  FROM INFORMATION_SCHEMA.COLUMNS "
                      " WHERE INFORMATION_SCHEMA.COLUMNS.TABLE_SCHEMA = "
                      "       DATABASE() "
                      "   AND INFORMATION_SCHEMA.COLUMNS.TABLE_NAME = "
                      "       :TABLENAME "
                      "   AND INFORMATION_SCHEMA.COLUMNS.COLUMN_NAME = "
                      "       :COLUMNNAME;";
        if (query.prepare(sql))
        {
            query.bindValue(":TABLENAME", table);
            query.bindValue(":COLUMNNAME", "function");
            if (query.exec() && query.next())
                result = true;
        }
    }
    return result;
}


/// \brief DBLoggerThread constructor
/// \param logger DatabaseLogger instance that this thread belongs to
DBLoggerThread::DBLoggerThread(DatabaseLogger *logger) :
    MThread("DBLogger"), m_logger(logger),
    m_queue(new QQueue<LoggingItem *>),
    m_wait(new QWaitCondition()), m_aborted(false)
{
}

/// \brief DBLoggerThread deconstructor.  Waits for the thread to finish, then
///        Empties what remains in the queue before deleting it.
DBLoggerThread::~DBLoggerThread()
{
    stop();
    wait();

    QMutexLocker qLock(&m_queueMutex);
    while (!m_queue->empty())
        m_queue->dequeue()->deleteItem();
    delete m_queue;
    delete m_wait;
    m_queue = NULL;
    m_wait = NULL;
}

/// \brief Start the thread.
void DBLoggerThread::run(void)
{
    RunProlog();

    // Wait a bit before we start logging to the DB..  If we wait too long,
    // then short-running tasks (like mythpreviewgen) will not log to the db
    // at all, and that's undesirable.
    while (true)
    {
        if ((m_aborted || (gCoreContext && m_logger->isDatabaseReady())))
            break;

        QMutexLocker locker(&m_queueMutex);
        m_wait->wait(locker.mutex(), 100);
    }

    if (!m_aborted)
    {
        // We want the query to be out of scope before the RunEpilog() so
        // shutdown occurs correctly as otherwise the connection appears still
        // in use, and we get a qWarning on shutdown.
        MSqlQuery *query = new MSqlQuery(MSqlQuery::InitCon());
        m_logger->prepare(*query);

        QMutexLocker qLock(&m_queueMutex);
        while (!m_aborted || !m_queue->isEmpty())
        {
            if (m_queue->isEmpty())
            {
                m_wait->wait(qLock.mutex(), 100);
                continue;
            }

            LoggingItem *item = m_queue->dequeue();
            if (!item)
                continue;

            qLock.unlock();

            if (item->message()[0] != '\0')
            {
                if (!m_logger->logqmsg(*query, item))
                {
                    qLock.relock();
                    m_queue->prepend(item);
                    m_wait->wait(qLock.mutex(), 100);
                    delete query;
                    query = new MSqlQuery(MSqlQuery::InitCon());
                    m_logger->prepare(*query);
                    continue;
                }
            }
            else
            {
                item->deleteItem();
            }

            qLock.relock();
        }

        delete query;

        qLock.unlock();
    }

    RunEpilog();
}

/// \brief Tell the thread to stop by setting the m_aborted flag.
void DBLoggerThread::stop(void)
{
    QMutexLocker qLock(&m_queueMutex);
    m_aborted = true;
    m_wait->wakeAll();
}



#ifndef _WIN32
/// \brief UNIX-side SIGHUP signal handler.  Hand it off to the Qt-size via
///        a socketpair so Qt code can safely be used.

void logSighup(int signum)
{
    (void)signum;
    char a = 1;
    int ret = ::write(sighupFd[0], &a, sizeof(a));
    (void)ret;
}
#endif


/// \brief LogServerThread constructor.
LogServerThread::LogServerThread() :
    MThread("LogServer"), m_zmqContext(NULL), m_zmqInSock(NULL),
    m_heartbeatTimer(NULL)
{
    moveToThread(qthread());
}

/// \brief LogServerThread destructor.
LogServerThread::~LogServerThread()
{
    stop();
    wait();
}

/// \brief Run the logging thread.  This thread reads from ZeroMQ (TCP:35327)
///        and handles distributing the LoggingItems to each logger instance
///        vi ZeroMQ (inproc).
void LogServerThread::run(void)
{
    RunProlog();

    logThreadFinished = false;
    QMutexLocker locker(&logThreadStartedMutex);

    qRegisterMetaType<QList<QByteArray> >("QList<QByteArray>");

    m_zmqContext = nzmqt::createDefaultContext(NULL);
    nzmqt::PollingZMQContext *ctx = static_cast<nzmqt::PollingZMQContext *>
                                        (m_zmqContext);
    ctx->setInterval(100);
    ctx->start();

    m_zmqInSock = m_zmqContext->createSocket(nzmqt::ZMQSocket::TYP_ROUTER);
    connect(m_zmqInSock, SIGNAL(messageReceived(const QList<QByteArray>&)),
            this, SLOT(receivedMessage(const QList<QByteArray>&)),
            Qt::QueuedConnection);
    m_zmqInSock->bindTo("tcp://127.0.0.1:35327");
    m_zmqInSock->bindTo("inproc://mylogs");

    logForwardThread = new LogForwardThread();
    logForwardThread->start();

    connect(logForwardThread, SIGNAL(pingClient(QString)),
            this, SLOT(pingClient(QString)), Qt::QueuedConnection);

    // cerr << "wake all" << endl;
    locker.unlock();
    logThreadStarted.wakeAll();
    // cerr << "unlock" << endl;
 
    msgsSinceHeartbeat = 0;
    m_heartbeatTimer = new QTimer(this);
    connect(m_heartbeatTimer, SIGNAL(timeout()), this, SLOT(checkHeartBeats()),
            Qt::QueuedConnection);
    m_heartbeatTimer->start(1000);

    exec();

    logThreadFinished = true;

    m_heartbeatTimer->stop();
    delete m_heartbeatTimer;

    m_zmqInSock->setLinger(0);
    m_zmqInSock->close();

    if (logForwardThread)
        logForwardThread->stop();
    delete logForwardThread;
    logForwardThread = NULL;

    delete m_zmqContext;

    RunEpilog();
}

/// \brief  Sends a ping message to the given client
/// \param clientId The clientID for the logging client we wish to ping
void LogServerThread::pingClient(QString clientId)
{
    LogMessage msg;
    // cout << "ping " << clientId.toLocal8Bit().constData() << endl;
    QByteArray clientBa = QByteArray::fromHex(clientId.toLocal8Bit());
    msg << clientBa << QByteArray("");
    m_zmqInSock->sendMessage(msg);
}


/// \brief  Handles heartbeat checking once a second.  If a client is not heard
///         from for at least 1 second, send it a heartbeat message which it
///         should send back.  If we haven't heard from it in 5s, shut down its
///         logging.
void LogServerThread::checkHeartBeats(void)
{
    qlonglong epoch;

    QMutexLocker lock(&logClientMapMutex);
    QMutexLocker lock2(&logClientToDelMutex);
    loggingGetTimeStamp(&epoch, NULL);

    // cout << "msgcount " << msgsSinceHeartbeat << endl;
    msgsSinceHeartbeat = 0;

    ClientMap::iterator it = logClientMap.begin();
    for( ; it != logClientMap.end(); ++it )
    {
        QString clientId        = it.key();
        LoggerListItem *logItem = it.value();
        qlonglong age = epoch - logItem->epoch;

        if (age > 5)
        {
            logClientToDel.append(clientId);
        }
        else
        {
            // cout << "age " << age << " " << clientId.toLocal8Bit().constData() << endl;
            pingClient(clientId);
        }
    }
}

/// \brief  Handles messages received from logging clients
/// \param  msg    The message received (can be multi-part)
void LogServerThread::receivedMessage(const QList<QByteArray> &msg)
{
    LogMessage *message = new LogMessage(msg);
    QMutexLocker lock(&logMsgListMutex);

    bool wasEmpty = logMsgList.isEmpty();
    logMsgList.append(message);

    if (wasEmpty)
        logMsgListNotEmpty.wakeAll();
}

/// \brief Stop the thread
void LogServerThread::stop(void)
{
    quit();
}

static QAtomicInt item_count;
static QAtomicInt malloc_count;

#define DEBUG_MEMORY 0
#if DEBUG_MEMORY
static int max_count = 0;
static QTime memory_time;
#endif



/// \brief  Entry point to start logging for the application.  This will
///         start up all of the threads needed.
/// \param  logfile Filename of the logfile to create.  Empty if no file.
/// \param  progress    non-zero if progress output will be sent to the console.
///                     This squelches all messages less important than LOG_ERR
///                     on the console
/// \param  quiet       quiet level requested (squelches all console output)
/// \param  facility    Syslog facility to use.  -1 to disable syslog output
/// \param  level       Minimum logging level to put into the logs
/// \param  dblog       true if database logging is requested
/// \param  propagate   true if the logfile path needs to be propagated to child
///                     processes.
void logServerStart(void)
{
    if (logServerThread && logServerThread->isRunning())
        return;

    if (!logServerThread)
        logServerThread = new LogServerThread();

    // cerr << "starting server" << endl;
    QMutexLocker locker(&logThreadStartedMutex);
    logServerThread->start();
    logThreadStarted.wait(locker.mutex());
    locker.unlock();
    // cerr << "done starting server" << endl;
}

/// \brief  Entry point for stopping logging for an application
void logServerStop(void)
{
    if (logServerThread)
    {
        logServerThread->stop();
        logServerThread->wait();
    }

    QMutexLocker locker(&loggerMapMutex);
    QMap<QString, LoggerBase *>::iterator it;
    for (it = loggerMap.begin(); it != loggerMap.end(); ++it)
    {
        it.value()->stopDatabaseAccess();
    }
}

void logServerWait(void)
{
    // cerr << "waiting" << endl;
    QMutexLocker locker(&logThreadStartedMutex);
    logThreadStarted.wait(locker.mutex());
    locker.unlock();
    // cerr << "done waiting" << endl;
}

void FileLogger::receivedMessage(const QList<QByteArray> &msg)
{
    // Filter on the clientId
    QByteArray clientBa = msg.first();
    QString clientId = QString(clientBa.toHex());

    {
        QMutexLocker locker(&logRevClientMapMutex);

        ClientList *clients = logRevClientMap.value(this, NULL);
        if (!clients || !clients->contains(clientId))
            return;
    }

    QByteArray json     = msg.at(1);
    LoggingItem *item = LoggingItem::create(json);
    logmsg(item);
    item->deleteItem();
}

#ifndef _WIN32
void SyslogLogger::receivedMessage(const QList<QByteArray> &msg)
{
    // Filter on the clientId
    QByteArray clientBa = msg.first();
    QString clientId = QString(clientBa.toHex());

    {
        QMutexLocker locker(&logRevClientMapMutex);

        ClientList *clients = logRevClientMap.value(this, NULL);
        if (!clients || !clients->contains(clientId))
            return;
    }

    QByteArray json     = msg.at(1);
    LoggingItem *item = LoggingItem::create(json);
    logmsg(item);
    item->deleteItem();
}

#else

// Windows doesn't have syslog support

void SyslogLogger::receivedMessage(const QList<QByteArray> &msg)
{
    (void)msg;
}

#endif

void DatabaseLogger::receivedMessage(const QList<QByteArray> &msg)
{
    // Filter on the clientId
    QByteArray clientBa = msg.first();
    QString clientId = QString(clientBa.toHex());

    {
        QMutexLocker locker(&logRevClientMapMutex);

        ClientList *clients = logRevClientMap.value(this, NULL);
        if (!clients || !clients->contains(clientId))
            return;
    }

    QByteArray json     = msg.at(1);
    LoggingItem *item = LoggingItem::create(json);
    logmsg(item);
}


/// \brief LogForwardThread constructor.
LogForwardThread::LogForwardThread() :
    MThread("LogForward"), m_aborted(false), m_zmqContext(NULL),
    m_zmqPubSock(NULL), m_sighupNotifier(NULL), m_shutdownTimer(NULL)
{
    moveToThread(qthread());
}

/// \brief LogForwardThread destructor.
LogForwardThread::~LogForwardThread()
{
    stop();
    wait();
}

/// \brief Run the log forwarding thread.  This thread reads from an internal
///        queue and handles distributing the LoggingItems to each logger
///        instance vi ZeroMQ (inproc).
void LogForwardThread::run(void)
{
    RunProlog();

#ifndef _WIN32
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sighupFd))
    {
        LOG(VB_GENERAL, LOG_ERR, "Couldn't create HUP socketpair");
        qApp->quit();
    }
    m_sighupNotifier = new QSocketNotifier(sighupFd[1], QSocketNotifier::Read,
                                           this);
    connect(m_sighupNotifier, SIGNAL(activated(int)),
            this, SLOT(handleSigHup()));

    /* Setup SIGHUP */
    LOG(VB_GENERAL, LOG_INFO, "Setup SIGHUP handler");
    struct sigaction sa;
    sa.sa_handler = logSighup;
    sigemptyset( &sa.sa_mask );
    sa.sa_flags = SA_RESTART;
    sigaction( SIGHUP, &sa, NULL );
#endif

    qRegisterMetaType<QList<QByteArray> >("QList<QByteArray>");

    m_zmqContext = nzmqt::createDefaultContext(NULL);
    nzmqt::PollingZMQContext *ctx = static_cast<nzmqt::PollingZMQContext *>
                                        (m_zmqContext);
    ctx->start();

    m_zmqPubSock = m_zmqContext->createSocket(nzmqt::ZMQSocket::TYP_PUB, this);
    m_zmqPubSock->bindTo("inproc://loggers");

    m_shutdownTimer = new QTimer(this);
    m_shutdownTimer->setSingleShot(true);
    connect(m_shutdownTimer, SIGNAL(timeout()),
            this, SLOT(shutdownTimerExpired()), Qt::QueuedConnection);
    LOG(VB_GENERAL, LOG_INFO, "Starting 5min shutdown timer");
    m_shutdownTimer->start(5*60*1000);

    while (!m_aborted)
    {
        qApp->processEvents(QEventLoop::AllEvents, 10);

        {
            QMutexLocker lock(&logMsgListMutex);
            if (logMsgList.isEmpty() &&
                !logMsgListNotEmpty.wait(lock.mutex(), 90))
            {
                continue;
            }

            int processed = 0;
            while (!logMsgList.isEmpty() && processed < 100)
            {
                processed++;
                LogMessage *msg = logMsgList.takeFirst();
                lock.unlock();
                forwardMessage(msg);
                delete msg;
                lock.relock();
            }
        }

        expireClients();
    }

    delete m_sighupNotifier;

#ifndef _WIN32
    ::close(sighupFd[0]);
    ::close(sighupFd[1]);

    /* Tear down SIGHUP */
    sa.sa_handler = SIG_DFL;
    sigemptyset( &sa.sa_mask );
    sa.sa_flags = SA_RESTART;
    sigaction( SIGHUP, &sa, NULL );
#endif

    m_zmqPubSock->setLinger(0);
    m_zmqPubSock->close();

    if (m_shutdownTimer)
    {
        if (m_shutdownTimer->isActive())
            m_shutdownTimer->stop();
        delete m_shutdownTimer;
        m_shutdownTimer = NULL;
    }

    LoggerList loggers;

    {
        QMutexLocker lock(&loggerMapMutex);
        loggers = loggerMap.values();
    }

    while (!loggers.isEmpty())
    {
        LoggerBase *logger = loggers.takeFirst();
        delete logger;
    }

    delete m_zmqContext;

    RunEpilog();
}


/// \brief  Fires off when no clients are left (other than the current daemon)
///         for 5 minutes.
void LogForwardThread::shutdownTimerExpired(void)
{
    m_shutdownTimer->stop();
    delete m_shutdownTimer;
    m_shutdownTimer = NULL;

    LOG(VB_GENERAL, LOG_INFO, "Shutting down because of idleness");
    msleep(500);
    qApp->quit();
}

/// \brief Expire any clients in the delete list
void LogForwardThread::expireClients(void)
{
    QMutexLocker lock(&logClientMapMutex);
    QMutexLocker lock2(&logRevClientMapMutex);
    QMutexLocker lock3(&logClientToDelMutex);

    while (!logClientToDel.isEmpty())
    {
        QString clientId = logClientToDel.takeFirst();
        logClientCount.deref();
        LOG(VB_GENERAL, LOG_INFO, QString("Expiring client %1 (#%2)")
            .arg(clientId).arg(logClientCount));
        LoggerListItem *item = logClientMap.take(clientId);
        LoggerList *list = item->list;
        delete item;

        while (!list->isEmpty())
        {
            LoggerBase *logger = list->takeFirst();
            ClientList *clientList = logRevClientMap.value(logger, NULL);
            if (!clientList || clientList->size() == 1)
            {
                if (clientList)
                {
                    logRevClientMap.remove(logger);
                    delete clientList;
                }
                delete logger;
                continue;
            }

            clientList->removeAll(clientId);
        }
        delete list;
    }

    // just this daemon left
    if (logClientCount == 1 && m_shutdownTimer && !m_shutdownTimer->isActive())
    {
        LOG(VB_GENERAL, LOG_INFO, "Starting 5min shutdown timer");
        m_shutdownTimer->start(5*60*1000);
    }
}

/// \brief  SIGHUP handler - reopen all open logfiles for logrollers
void LogForwardThread::handleSigHup(void)
{
#ifndef _WIN32
    m_sighupNotifier->setEnabled(false);

    char tmp;
    int ret = ::read(sighupFd[1], &tmp, sizeof(tmp));
    (void)ret;

    LOG(VB_GENERAL, LOG_INFO, "SIGHUP received, rolling log files.");

    /* SIGHUP was sent.  Close and reopen debug logfiles */
    QMutexLocker locker(&loggerMapMutex);
    QMap<QString, LoggerBase *>::iterator it;
    for (it = loggerMap.begin(); it != loggerMap.end(); ++it)
    {
        it.value()->reopen();
    }

    m_sighupNotifier->setEnabled(true);
#endif
}

void LogForwardThread::forwardMessage(LogMessage *msg)
{
#ifdef DUMP_PACKET
    QList<QByteArray>::const_iterator it = msg->begin();
    int i = 0;
    for (; it != msg->end(); ++it, i++)
    {
        QByteArray buf = *it;
        cout << i << ":\t" << buf.size() << endl << "\t" 
             << buf.toHex().constData() << endl << "\t"
             << buf.constData() << endl;
    }
#endif

    msgsSinceHeartbeat.ref();

    // First section is the client id
    QByteArray clientBa = msg->first();
    QString clientId = QString(clientBa.toHex());

    QByteArray json     = msg->at(1);

    if (json.size() == 0)
    {
        // This is either a ping response or a first gasp
        QMutexLocker lock(&logClientMapMutex);
        LoggerListItem *logItem = logClientMap.value(clientId, NULL);
        if (!logItem)
        {
            // Send an initial ping so the client knows we are in the house
            emit pingClient(clientId);
        }
        else
        {
            // cout << "pong " << clientId.toLocal8Bit().constData() << endl;
            loggingGetTimeStamp(&logItem->epoch, NULL);
        }
        return;
    }

    try
    {
        QMutexLocker lock(&logClientMapMutex);
        LoggerListItem *logItem = logClientMap.value(clientId, NULL);

        // cout << "msg  " << clientId.toLocal8Bit().constData() << endl;
        if (logItem)
        {
            loggingGetTimeStamp(&logItem->epoch, NULL);
        }
        else
        {
            LoggingItem *item = LoggingItem::create(json);

            logClientCount.ref();
            LOG(VB_GENERAL, LOG_INFO, QString("New Client: %1 (#%2)")
                .arg(clientId).arg(logClientCount));

            if (logClientCount > 1 && m_shutdownTimer &&
                m_shutdownTimer->isActive())
            {
                LOG(VB_GENERAL, LOG_INFO, "Aborting shutdown timer");
                m_shutdownTimer->stop();
            }

            QMutexLocker lock2(&loggerMapMutex);
            QMutexLocker lock3(&logRevClientMapMutex);

            // Need to find or create the loggers
            LoggerList *loggers = new LoggerList;
            LoggerBase *logger;

            // FileLogger from logFile
            QString logfile = item->logFile();
            logfile.detach();
            if (!logfile.isEmpty())
            {
                ClientList *clients;
                logger = loggerMap.value(logfile, NULL);
                if (!logger)
                {
                    // Need to add a new FileLogger
                    lock2.unlock();
                    // inserts into loggerMap
                    logger = new FileLogger(logfile.toLocal8Bit().constData());
                    //logger->moveToThread(logForwardThread->qthread());
                    lock2.relock();
                    logger->setupZMQSocket();

                    clients = new ClientList;
                    logRevClientMap.insert(logger, clients);
                }
                else
                {
                    clients = logRevClientMap.value(logger);
                    if (!clients)
                    {
                        clients = new ClientList;
                        logRevClientMap.insert(logger, clients);
                    }
                }
                clients->insert(0, clientId);
                loggers->insert(0, logger);
            }

#ifndef _WIN32
            // SyslogLogger from facility
            int facility = item->facility();
            if (facility > 0)
            {
                ClientList *clients;
                logger = loggerMap.value(QString(""), NULL);
                if (!logger)
                {
                    // Need to add a new SyslogLogger
                    lock2.unlock();
                    logger = new SyslogLogger; // inserts into loggerMap
                    //logger->moveToThread(logForwardThread->qthread());
                    lock2.relock();
                    logger->setupZMQSocket();

                    clients = new ClientList;
                    logRevClientMap.insert(logger, clients);
                }
                else
                {
                    clients = logRevClientMap.value(logger);
                    if (!clients)
                    {
                        clients = new ClientList;
                        logRevClientMap.insert(logger, clients);
                    }
                }
                clients->insert(0, clientId);
                loggers->insert(0, logger);
            }
#endif

            // DatabaseLogger from table
            QString table = item->table();
            if (!table.isEmpty())
            {
                ClientList *clients;
                logger = loggerMap.value(table, NULL);
                if (!logger)
                {
                    // Need to add a new DatabaseLogger
                    lock2.unlock();
                    // inserts into loggerMap
                    logger =
                        new DatabaseLogger(table.toLocal8Bit().constData());
                    //logger->moveToThread(logForwardThread->qthread());
                    lock2.relock();
                    logger->setupZMQSocket();

                    clients = new ClientList;
                    logRevClientMap.insert(logger, clients);
                }
                else
                {
                    clients = logRevClientMap.value(logger);
                    if (!clients)
                    {
                        clients = new ClientList;
                        logRevClientMap.insert(logger, clients);
                    }
                }
                clients->insert(0, clientId);
                loggers->insert(0, logger);
            }

            logItem = new LoggerListItem;
            loggingGetTimeStamp(&logItem->epoch, NULL);
            logItem->list = loggers;
            logClientMap.insert(clientId, logItem);

            item->deleteItem();
        }
    }
    catch (nzmqt::ZMQException e)
    {
        LOG(VB_GENERAL, LOG_ERR, QString("Exception during socket setup: %1")
            .arg(e.what()));
    }


    m_zmqPubSock->sendMessage(*msg);
}

/// \brief Stop the thread by setting the abort flag
void LogForwardThread::stop(void)
{
    m_aborted = true;
}

/*
 * vim:ts=4:sw=4:ai:et:si:sts=4
 */
