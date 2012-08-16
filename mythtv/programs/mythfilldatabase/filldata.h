#ifndef _FILLDATA_H_
#define _FILLDATA_H_

// C++ headers
#include <vector>
using namespace std;

// Qt headers
#include <QString>

// libmythtv headers
#include "datadirect.h"
#include "programdata.h"

// filldata headers
#include "channeldata.h"
#include "xmltvparser.h"
#include "icondata.h"

#define REFRESH_MAX 21

// helper functions to update mfdb status fields in settings
bool updateLastRunEnd(MSqlQuery &query);
bool updateLastRunStart(MSqlQuery &query);
bool updateLastRunStatus(MSqlQuery &query, QString &status);

struct Source
{
    Source() : id(0), name(), xmltvgrabber(), userid(), password(), lineupid(),
        version(0), modified(),
        xmltvgrabber_baseline(false), xmltvgrabber_manualconfig(false),
        xmltvgrabber_cache(false), xmltvgrabber_prefmethod() {}
    int id;
    QString name;
    QString xmltvgrabber;
    QString userid;
    QString password;
    QString lineupid;
    int version;
    QString modified;
    bool    xmltvgrabber_baseline;
    bool    xmltvgrabber_manualconfig;
    bool    xmltvgrabber_cache;
    QString xmltvgrabber_prefmethod;
    vector<int> dd_dups;
};
typedef vector<Source> SourceList;

struct QAM
{
    QAM(): frequency(), virtualchannel(), modulation(), program() {}
    QString frequency;
    QString virtualchannel;
    QString modulation;
    QString program;
};

class FillData
{
public:
    FillData() :
        raw_lineup(0),                  maxDays(0),
        interrupted(false),             endofdata(false),
        refresh_tba(true),              dd_grab_all(false),
        dddataretrieved(false),
        need_post_grab_proc(true),      only_update_channels(false),
        channel_update_run(false),      refresh_all(false)
    {
        SetRefresh(1, true);
    }

    void SetRefresh(int day, bool set);

    void DataDirectStationUpdate(Source source, bool update_icons = true);
    bool DataDirectUpdateChannels(Source source);
    bool GrabDDData(Source source, int poffset,
                    QDate pdate, int ddSource);
    bool GrabDataFromFile(int id, QString &filename);
    bool GrabData(Source source, int offset, QDate *qCurrentDate = 0);
    bool GrabDataFromDDFile(int id, int offset, const QString &filename,
                            const QString &lineupid, QDate *qCurrentDate = 0);

    QString GetSDLoginRandhash(Source source);
    bool DownloadSDFiles(QString randhash, QString whattoget, Source source);
    bool InsertSDDataintoDatabase(Source source);
    int is_SDHeadendVersionUpdated(Source source);
    bool getSchedulesDirectStatusMessages(QString randhash);
    int UpdateChannelTablefromSD(Source source);
    bool ProcessXMLTV_URL(Source source);

    bool Run(SourceList &sourcelist);
    ChanInfo *xawtvChannel(QString &id, QString &channel, QString &fine);
    void readXawtvChannels(int id, QString xawrcfile);

    enum
    {
        kRefreshClear = 0xFFFF0,
        kRefreshAll   = 0xFFFF1,
    };

public:
    ProgramData         prog_data;
    ChannelData         chan_data;
    XMLTVParser         xmltv_parser;
    IconData            icon_data;
    DataDirectProcessor ddprocessor;

    QString logged_in;
    QString lastdduserid;
    QString graboptions;
    int     raw_lineup;
    uint    maxDays;

    bool    interrupted;
    bool    endofdata;
    bool    refresh_tba;
    bool    dd_grab_all;
    bool    dddataretrieved;
    bool    need_post_grab_proc;
    bool    only_update_channels;
    bool    channel_update_run;

private:
    QMap<uint, bool>     refresh_day;
    bool                refresh_all;
    mutable QStringList fatalErrors;
    int     new_version;
    QString new_modified;
};

#endif // _FILLDATA_H_
