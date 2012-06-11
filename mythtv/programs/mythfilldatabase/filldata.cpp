// POSIX headers
#include <unistd.h>

// Std C headers
#include <cstdlib>
#include <ctime>

// C++ headers
#include <fstream>
using namespace std;

// Qt headers
#include <QTextStream>
#include <QDateTime>
#include <QFile>
#include <QList>
#include <QMap>
#include <QDir>
//#include <QJson>
#include <QVariant>
#include <QDebug>
#include <qjson/parser.h>

// MythTV headers
#include "mythmiscutil.h"
#include "exitcodes.h"
#include "mythlogging.h"
#include "mythdbcon.h"
#include "compat.h"
#include "mythdate.h"
#include "mythdirs.h"
#include "mythdb.h"
#include "mythsystem.h"
#include "mythdownloadmanager.h"
#include "videosource.h" // for is_grabber..

// filldata headers
#include "filldata.h"

// fillutil header
#include "fillutil.h" // for uncompress routine

#define LOC QString("FillData: ")
#define LOC_WARN QString("FillData, Warning: ")
#define LOC_ERR QString("FillData, Error: ")

bool updateLastRunEnd(MSqlQuery &query)
{
    QDateTime qdtNow = MythDate::current();
    query.prepare("UPDATE settings SET data = :ENDTIME "
                  "WHERE value='mythfilldatabaseLastRunEnd'");

    query.bindValue(":ENDTIME", qdtNow);

    if (!query.exec())
    {
        MythDB::DBError("updateLastRunEnd", query);
        return false;
    }

    return true;
}

bool updateLastRunStart(MSqlQuery &query)
{
    QDateTime qdtNow = MythDate::current();
    query.prepare("UPDATE settings SET data = :STARTTIME "
                  "WHERE value='mythfilldatabaseLastRunStart'");

    query.bindValue(":STARTTIME", qdtNow);

    if (!query.exec())
    {
        MythDB::DBError("updateLastRunStart", query);
        return false;
    }

    return true;
}

bool updateLastRunStatus(MSqlQuery &query, QString &status)
{
    query.prepare("UPDATE settings SET data = :STATUS "
                  "WHERE value='mythfilldatabaseLastRunStatus'");

    query.bindValue(":STATUS", status);

    if (!query.exec())
    {
        MythDB::DBError("updateLastRunStatus", query);
        return false;
    }

    return true;
}

void FillData::SetRefresh(int day, bool set)
{
    if (kRefreshClear == day)
    {
        refresh_all = set;
        refresh_day.clear();
    }
    else if (kRefreshAll == day)
    {
        refresh_all = set;
    }
    else
    {
        refresh_day[(uint)day] = set;
    }
}

// DataDirect stuff
void FillData::DataDirectStationUpdate(Source source, bool update_icons)
{
    DataDirectProcessor::UpdateStationViewTable(source.lineupid);

    bool insert_channels = chan_data.insert_chan(source.id);
    int new_channels = DataDirectProcessor::UpdateChannelsSafe(
                           source.id, insert_channels, chan_data.filter_new_channels);

    //  User must pass "--do-channel-updates" for these updates
    if (chan_data.channel_updates)
    {
        DataDirectProcessor::UpdateChannelsUnsafe(
            source.id, chan_data.filter_new_channels);
    }

    // TODO delete any channels which no longer exist in listings source

    if (update_icons)
        icon_data.UpdateSourceIcons(source.id);

    // Unselect channels not in users lineup for DVB, HDTV
    if (!insert_channels && (new_channels > 0) &&
        is_grabber_labs(source.xmltvgrabber))
    {
        bool ok0 = (logged_in == source.userid);
        bool ok1 = (raw_lineup == source.id);

        if (!ok0)
        {
            LOG(VB_GENERAL, LOG_INFO,
                "Grabbing login cookies for listing update");
            ok0 = ddprocessor.GrabLoginCookiesAndLineups();
        }

        if (ok0 && !ok1)
        {
            LOG(VB_GENERAL, LOG_INFO, "Grabbing listing for listing update");
            ok1 = ddprocessor.GrabLineupForModify(source.lineupid);
        }

        if (ok1)
        {
            ddprocessor.UpdateListings(source.id);
            LOG(VB_GENERAL, LOG_INFO,
                QString("Removed %1 channel(s) from lineup.")
                .arg(new_channels));
        }
    }
}

bool FillData::DataDirectUpdateChannels(Source source)
{
    if (get_datadirect_provider(source.xmltvgrabber) >= 0)
    {
        ddprocessor.SetListingsProvider(
            get_datadirect_provider(source.xmltvgrabber));
    }
    else
    {
        LOG(VB_GENERAL, LOG_ERR, LOC +
            "We only support DataDirectUpdateChannels with "
            "TMS Labs and Schedules Direct.");
        return false;
    }

    ddprocessor.SetUserID(source.userid);
    ddprocessor.SetPassword(source.password);

    bool ok = true;

    if (!is_grabber_labs(source.xmltvgrabber))
    {
        ok = ddprocessor.GrabLineupsOnly();
    }
    else
    {
        ok = ddprocessor.GrabFullLineup(
                 source.lineupid, true, chan_data.insert_chan(source.id)/*only sel*/);
        logged_in  = source.userid;
        raw_lineup = source.id;
    }

    if (ok)
        DataDirectStationUpdate(source, false);

    return ok;
}

bool FillData::GrabDDData(Source source, int poffset,
                          QDate pdate, int ddSource)
{
    if (source.dd_dups.empty())
        ddprocessor.SetCacheData(false);
    else
    {
        LOG(VB_GENERAL, LOG_INFO,
            QString("This DataDirect listings source is "
                    "shared by %1 MythTV lineups")
            .arg(source.dd_dups.size() + 1));

        if (source.id > source.dd_dups[0])
        {
            LOG(VB_GENERAL, LOG_NOTICE,
                "We should use cached data for this one");
        }
        else if (source.id < source.dd_dups[0])
        {
            LOG(VB_GENERAL, LOG_NOTICE,
                "We should keep data around after this one");
        }

        ddprocessor.SetCacheData(true);
    }

    ddprocessor.SetListingsProvider(ddSource);
    ddprocessor.SetUserID(source.userid);
    ddprocessor.SetPassword(source.password);

    bool needtoretrieve = true;

    if (source.userid != lastdduserid)
        dddataretrieved = false;

    if (dd_grab_all && dddataretrieved)
        needtoretrieve = false;

    MSqlQuery query(MSqlQuery::DDCon());
    QString status = QObject::tr("currently running.");

    updateLastRunStart(query);

    if (needtoretrieve)
    {
        LOG(VB_GENERAL, LOG_INFO, "Retrieving datadirect data.");

        if (dd_grab_all)
        {
            LOG(VB_GENERAL, LOG_INFO, "Grabbing ALL available data.");

            if (!ddprocessor.GrabAllData())
            {
                LOG(VB_GENERAL, LOG_ERR, "Encountered error in grabbing data.");
                return false;
            }
        }
        else
        {
            QDateTime fromdatetime =
                QDateTime(pdate, QTime(0, 0), Qt::UTC).addDays(poffset);
            QDateTime todatetime = fromdatetime.addDays(1);

            LOG(VB_GENERAL, LOG_INFO, QString("Grabbing data for %1 offset %2")
                .arg(pdate.toString())
                .arg(poffset));
            LOG(VB_GENERAL, LOG_INFO, QString("From %1 to %2 (UTC)")
                .arg(fromdatetime.toString(Qt::ISODate))
                .arg(todatetime.toString(Qt::ISODate)));

            if (!ddprocessor.GrabData(fromdatetime, todatetime))
            {
                LOG(VB_GENERAL, LOG_ERR, "Encountered error in grabbing data.");
                return false;
            }
        }

        dddataretrieved = true;
        lastdduserid = source.userid;
    }
    else
    {
        LOG(VB_GENERAL, LOG_INFO,
            "Using existing grabbed data in temp tables.");
    }

    LOG(VB_GENERAL, LOG_INFO,
        QString("Grab complete.  Actual data from %1 to %2 (UTC)")
        .arg(ddprocessor.GetDDProgramsStartAt().toString(Qt::ISODate))
        .arg(ddprocessor.GetDDProgramsEndAt().toString(Qt::ISODate)));

    updateLastRunEnd(query);

    LOG(VB_GENERAL, LOG_INFO, "Main temp tables populated.");

    if (!channel_update_run)
    {
        LOG(VB_GENERAL, LOG_INFO, "Updating MythTV channels.");
        DataDirectStationUpdate(source);
        LOG(VB_GENERAL, LOG_INFO, "Channels updated.");
        channel_update_run = true;
    }

#if 0
    LOG(VB_GENERAL, LOG_INFO, "Creating program view table...");
#endif
    DataDirectProcessor::UpdateProgramViewTable(source.id);
#if 0
    LOG(VB_GENERAL, LOG_INFO, "Finished creating program view table...");
#endif

    query.prepare("SELECT count(*) from dd_v_program;");

    if (query.exec() && query.next())
    {
        if (query.value(0).toInt() < 1)
        {
            LOG(VB_GENERAL, LOG_INFO, "Did not find any new program data.");
            return false;
        }
    }
    else
    {
        LOG(VB_GENERAL, LOG_ERR, "Failed testing program view table.");
        return false;
    }

    LOG(VB_GENERAL, LOG_INFO, "Clearing data for source.");
    QDateTime from = ddprocessor.GetDDProgramsStartAt();
    QDateTime to = ddprocessor.GetDDProgramsEndAt();

    LOG(VB_GENERAL, LOG_INFO, QString("Clearing from %1 to %2 (localtime)")
        .arg(from.toLocalTime().toString(Qt::ISODate))
        .arg(to.toLocalTime().toString(Qt::ISODate)));
    ProgramData::ClearDataBySource(source.id, from, to, true);
    LOG(VB_GENERAL, LOG_INFO, "Data for source cleared.");

    LOG(VB_GENERAL, LOG_INFO, "Updating programs.");
    DataDirectProcessor::DataDirectProgramUpdate();
    LOG(VB_GENERAL, LOG_INFO, "Program table update complete.");

    return true;
}

// XMLTV stuff
bool FillData::GrabDataFromFile(int id, QString &filename)
{
    QList<ChanInfo> chanlist;
    QMap<QString, QList<ProgInfo> > proglist;

    if (!xmltv_parser.parseFile(filename, &chanlist, &proglist))
        return false;

    chan_data.handleChannels(id, &chanlist);
    icon_data.UpdateSourceIcons(id);

    if (proglist.count() == 0)
    {
        LOG(VB_GENERAL, LOG_INFO, "No programs found in data.");
        endofdata = true;
    }
    else
    {
        prog_data.HandlePrograms(id, proglist);
    }

    return true;
}

bool FillData::GrabData(Source source, int offset, QDate *qCurrentDate)
{
    QString xmltv_grabber = source.xmltvgrabber;

    int dd_provider = get_datadirect_provider(xmltv_grabber);

    if (dd_provider >= 0)
    {
        if (!GrabDDData(source, offset, *qCurrentDate, dd_provider))
        {
            QStringList errors = ddprocessor.GetFatalErrors();

            for (int i = 0; i < errors.size(); i++)
                fatalErrors.push_back(errors[i]);

            return false;
        }

        return true;
    }

    const QString templatename = "/tmp/mythXXXXXX";

    const QString tempfilename = createTempFile(templatename);

    if (templatename == tempfilename)
    {
        fatalErrors.push_back("Failed to create temporary file.");
        return false;
    }

    QString filename = QString(tempfilename);

    QString home = QDir::homePath();

    QString configfile;

    MSqlQuery query1(MSqlQuery::InitCon());
    query1.prepare("SELECT configpath FROM videosource"
                   " WHERE sourceid = :ID AND configpath IS NOT NULL");
    query1.bindValue(":ID", source.id);

    if (!query1.exec())
    {
        MythDB::DBError("FillData::grabData", query1);
        return false;
    }

    if (query1.next())
        configfile = query1.value(0).toString();
    else
        configfile = QString("%1/%2.xmltv").arg(GetConfDir())
                     .arg(source.name);

    LOG(VB_GENERAL, LOG_INFO,
        QString("XMLTV config file is: %1").arg(configfile));

    QString command = QString("nice %1 --config-file '%2' --output %3")
                      .arg(xmltv_grabber).arg(configfile).arg(filename);

    // The one concession to grabber specific behaviour.
    // Will be removed when the grabber allows.
    if (xmltv_grabber == "tv_grab_jp")
    {
        command += QString(" --enable-readstr");
        xmltv_parser.isJapan = true;
    }
    else if (source.xmltvgrabber_prefmethod != "allatonce")
    {
        // XMLTV Docs don't recommend grabbing one day at a
        // time but the current MythTV code is heavily geared
        // that way so until it is re-written behave as
        // we always have done.
        command += QString(" --days 1 --offset %1").arg(offset);
    }

    if (!VERBOSE_LEVEL_CHECK(VB_XMLTV, LOG_ANY))
        command += " --quiet";

    // Append additional arguments passed to mythfilldatabase
    // using --graboptions
    if (!graboptions.isEmpty())
    {
        command += graboptions;
        LOG(VB_XMLTV, LOG_INFO,
            QString("Using graboptions: %1").arg(graboptions));
    }

    MSqlQuery query(MSqlQuery::InitCon());
    QString status = QObject::tr("currently running.");

    updateLastRunStart(query);
    updateLastRunStatus(query, status);

    LOG(VB_XMLTV, LOG_INFO, QString("Grabber Command: %1").arg(command));

    LOG(VB_XMLTV, LOG_INFO,
        "----------------- Start of XMLTV output -----------------");

    unsigned int systemcall_status;

    systemcall_status = myth_system(command, kMSRunShell);
    bool succeeded = (systemcall_status == GENERIC_EXIT_OK);

    LOG(VB_XMLTV, LOG_INFO,
        "------------------ End of XMLTV output ------------------");

    updateLastRunEnd(query);

    status = QObject::tr("Successful.");

    if (!succeeded)
    {
        if (systemcall_status == GENERIC_EXIT_KILLED)
        {
            interrupted = true;
            status =
                QString(QObject::tr("FAILED: xmltv ran but was interrupted."));
        }
        else
        {
            status =
                QString(QObject::tr("FAILED: xmltv returned error code %1."))
                .arg(systemcall_status);
            LOG(VB_GENERAL, LOG_ERR, LOC +
                QString("xmltv returned error code %1")
                .arg(systemcall_status));
        }
    }

    updateLastRunStatus(query, status);

    succeeded &= GrabDataFromFile(source.id, filename);

    QFile thefile(filename);
    thefile.remove();

    return succeeded;
}

bool FillData::GrabDataFromDDFile(
    int id, int offset, const QString &filename,
    const QString &lineupid, QDate *qCurrentDate)
{
    QDate *currentd = qCurrentDate;
    QDate qcd = MythDate::current().date();

    if (!currentd)
        currentd = &qcd;

    ddprocessor.SetInputFile(filename);
    Source s;
    s.id = id;
    s.xmltvgrabber = "datadirect";
    s.userid = "fromfile";
    s.password = "fromfile";
    s.lineupid = lineupid;

    return GrabData(s, offset, currentd);
}


// Schedules Direct login
QString FillData::GetSDLoginRandhash(Source source)
{
    QString randhash = "";
    QString username, password;
    username = source.userid;
    password = source.password;

    QString loginurl = "http://10.244.23.50/schedulesdirect/login.php";
    //    QString loginurl = "http://rkulagow.schedulesdirect.org/schedulesdirect/login.php";

    LOG(VB_GENERAL, LOG_INFO, "Getting randhash from Schedules Direct");
    MythDownloadManager *manager = GetMythDownloadManager();

    QByteArray tempdata, postdata;
    tempdata += "username=";
    tempdata += username;
    tempdata += "&password=";
    tempdata += password;
    tempdata += "&submit=Log+In";

    postdata = tempdata.toPercentEncoding("&=+");

    QByteArray header = "Content-Type";
    QByteArray value = "application/x-www-form-urlencoded";

    if (!manager->postAuth(loginurl, &postdata, NULL, NULL, &header, &value))
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + QString("Could not post auth credentials to Schedules Direct."));
        return QString("error");
    }

    // Next part is just for debugging
    /*    QString randhashFile = QString("/tmp/sd_randhash");
        QFile file(randhashFile);
        file.open(QIODevice::WriteOnly);
        file.write(postdata);
        file.close();
    */

    QRegExp rx("randhash: ([a-z0-9]+)");

    if (rx.indexIn(postdata) != -1)
    {
        randhash = rx.cap(1);
        LOG(VB_GENERAL, LOG_INFO, QString("randhash is %1").arg(randhash));
        return randhash;
    }
    else
    {
        LOG(VB_GENERAL, LOG_ERR, QString("Could not decode randhash."));
        return "error";
    }

}

// Schedules Direct download XMLID files.
bool FillData::DownloadSDFiles(QString randhash)
{
    // Download all the unique XMLIDs
    QString xmltvid, url, destfile;

    //QString urlbase = "http://rkulagow.schedulesdirect.org/schedulesdirect/process.php";
    QString urlbase = "http://10.244.23.50/schedulesdirect/process.php";

    QByteArray dl_file;

    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare(
        "SELECT distinct(xmltvid) FROM channel"
    );

    if (!query.exec())
    {
        MythDB::DBError("FillData::grabData", query);
        return false;
    }

    while (query.next())
        // We're going to update all chanid's in the database that use this particular XMLID.
    {
        xmltvid = query.value(0).toString();
        url = urlbase + "?command=get&p1=schedule&p2=" + xmltvid + "&rand=" + randhash;
        destfile = "/tmp/" + xmltvid + "_sched.txt";
        GetMythDownloadManager()->download(url, &dl_file, false);
        QFile file(destfile);

        if (file.open(QIODevice::WriteOnly))
        {
            file.write(gUncompress(dl_file));
            file.close();
        }
        else
        {
            LOG(VB_GENERAL, LOG_ERR, LOC + QString("Could not create file %1").arg(destfile));
            return false;
        }
    }

    return true;
}

bool FillData::getSchedulesDirectStatusMessages(QString randhash)
{
    //QString urlbase = "http://rkulagow.schedulesdirect.org/schedulesdirect/process.php";
    QString urlbase = "http://10.244.23.50/schedulesdirect/process.php";
    QString url;
    QString destfile;
    QDateTime qdtNow = MythDate::current();

    url = urlbase + "?command=get&p1=status&rand=$randhash";
    destfile = "/tmp/" + qdtNow.toLocalTime().toString(Qt::ISODate) + "-status.txt";

    GetMythDownloadManager()->download(url, destfile, false);

}



// Schedules Direct check for lineup update
bool FillData::is_SDHeadendVersionUpdated(Source source)
{
    //QString urlbase = "http://rkulagow.schedulesdirect.org/schedulesdirect/process.php";
    QString urlbase = "http://10.244.23.50/schedulesdirect/process.php";
    QString lineup = source.lineupid;
    int db_version = source.version;
    QString db_modified = source.modified;
    QString destfile;
    QByteArray lineupdata;

    qDebug() << "lineup is " << lineup << "db version is " << db_version << "modified is " << db_modified;

    /*
    * We don't specify the randhash because we don't need to. The lineup
    * function at Schedules Direct is open so that it can be used by the QAM
    * scanner.
    */
    QString url = urlbase + "?command=get&p1=lineup&p2=" + lineup;
    destfile = "/tmp/" + lineup + ".txt";
    GetMythDownloadManager()->download(url, &lineupdata, false);

    QFile file(destfile);
    file.open(QIODevice::WriteOnly);
    file.write(gUncompress(lineupdata));
    file.close();

    QRegExp rx("randhash: ([a-z0-9]+)");

    /*
        if (rx.indexIn(postdata) != -1)
        {
            randhash = rx.cap(1);
            LOG(VB_GENERAL, LOG_INFO, QString("randhash is %1").arg(randhash));
            return true;
        }
        else
        {
            LOG(VB_GENERAL, LOG_INFO, QString("Could not decode randhash."));
            return false;
        }
    */

    return false;

}


// Schedules Direct json-formatted data
bool FillData::InsertSDDataintoDatabase(
    int id, const QString &lineupid)
{
    // Information relating to schedules
    bool subject_to_blackout, educational, time_approximate;
    bool joined_in_progress, left_in_progress;
    bool sex_rating, dialog_rating, tv_rating, violence_rating;
    bool fv_rating, lang_rating;
    bool is_cc, is_stereo, dolby;
    bool cable_in_the_classroom;
    bool is_new, is_enhanced, is_3d, is_hdtv, is_letterboxed, has_dvs;
    QString network_syndicated_source, network_syndicated_type;
    QString live_tape_delay;
    QString is_premiere_or_finale;
    QString sched_prog_id;
    QString air_date, air_time;
    QString season, episode;
    int duration, part_num, num_parts;

    // Information relating to programs.
    QString title, reduced_title1, reduced_title2, reduced_title3, reduced_title4;
    QString epi_title, alt_title;
    QString descr, reduced_descr1, reduced_descr2, reduced_descr3, descr_500;
    QString descr2, descr2_reduced;
    QString descr_lang_id;
    QString source_type, show_type;
    QString syn_epi_num, alt_syn_epi_num;
    QString color_code;
    QString orig_air_date;
    bool made_for_tv;

    QString status; //used for status messages logged to the database.

    MSqlQuery startstopstatus_query(MSqlQuery::InitCon());

    updateLastRunStart(startstopstatus_query);

    LOG(VB_GENERAL, LOG_INFO, "Reading schedule files.");
    status = "Reading schedule files.";

    updateLastRunStatus(startstopstatus_query, status);

    QDir dir;
    dir = QDir("/tmp");
    QStringList files;
    QString searchfor = "*_sched.txt";

    files = dir.entryList(QStringList(searchfor), QDir::Files | QDir::NoSymLinks);

    QStringListIterator j(files);

    while (j.hasNext())
    {

        QString filename = j.next().toLocal8Bit();

        QFile inputfile("/tmp/" + filename);

        if (!inputfile.open(QIODevice::ReadOnly))
        {
            qDebug() << "Couldn't open filename: " << filename;
            QString status = "Failed to open filename " + filename;
            updateLastRunStatus(startstopstatus_query, status);
            return false;
        }

        QJson::Parser parser;
        bool ok;

        LOG(VB_GENERAL, LOG_INFO, QString("Loaded file: %1").arg(filename));

        QMap<QString, QString> schedule;
        QMap<QString, QString> program_information;
        QString line;

        while (!inputfile.atEnd())
        {
            // Read everything into appropriate QMaps first. The largest data file only has around 4000 lines, so not huge.
            line = inputfile.readLine();
            QVariantMap result = parser.parse(line.toLocal8Bit(), &ok).toMap(); //json parser wants QByteArray

            if (!ok)
            {
                printf("line %d: %s\n", parser.errorLine(), parser.errorString().toUtf8().data());
                return false;
            }

            if (result["datatype"].toString() == "schedule")
            {
                schedule[result["prog_id"].toString()] = line;
            }
            else
            {
                program_information[result["prog_id"].toString()] = line;
            }
        } // done reading in all the data.

        inputfile.close();

        QMapIterator<QString, QString> i(schedule);

        while (i.hasNext())
        {
            // Iterate through all the schedule information for this XMLID.
            i.next();
            // qDebug() << i.key() << ": " << i.value();


            QVariantMap result = parser.parse(i.value().toLocal8Bit(), &ok).toMap();

            if (!ok)
            {
                printf("line %d: %s\n", parser.errorLine(), parser.errorString().toUtf8().data());
                return false;
            }

            air_date = result["air_date"].toString();
            air_time = result["air_time"].toString();
            duration = result["duration"].toInt();
            season = result["season"].toString();
            episode = result["episode"].toString();
            part_num = result["part_num"].toInt();
            num_parts = result["num_parts"].toInt();

            subject_to_blackout = result["subject_to_blackout"].toBool();
            educational = result["educational"].toBool();
            time_approximate = result["time_approximate"].toBool();
            joined_in_progress = result["joined_in_progress"].toBool();
            left_in_progress = result["left_in_progress"].toBool();
            sex_rating = result["sex_rating"].toBool();
            dialog_rating = result["dialog_rating"].toBool();
            tv_rating = result["tv_rating"].toBool();
            violence_rating = result["violence_rating"].toBool();
            fv_rating = result["fv_rating"].toBool();
            lang_rating = result["lang_rating"].toBool();
            is_cc = result["cc"].toBool();
            is_stereo = result["stereo"].toBool();
            dolby = result["dolby"].toBool();
            cable_in_the_classroom = result["cable_in_the_classroom"].toBool();
            is_new = result["new"].toBool();
            is_enhanced = result["enhanced"].toBool();
            is_3d = result["3d"].toBool();
            is_hdtv = result["hdtv"].toBool();
            is_letterboxed = result["letterbox"].toBool();
            network_syndicated_source = result["net_syn_source"].toString();
            network_syndicated_type = result["net_syn_type"].toString();
            live_tape_delay = result["live_tape_delay"].toString();
            is_premiere_or_finale = result["premiere_finale"].toString();

            QDateTime UTCdt_start = QDateTime::fromString(air_date + " " + air_time, Qt::ISODate);
            QDateTime UTCdt_end = UTCdt_start.addSecs(duration);

            QVariantMap prog_info = parser.parse(program_information[i.key()].toLocal8Bit(), &ok).toMap();

            // Use the key (program ID) from the schedule to get the program information for that timeslot.
            if (!ok)
            {
                printf("line %d: %s\n", parser.errorLine(), parser.errorString().toUtf8().data());
                return false;
            }

            title = prog_info["title"].toString();
            epi_title = prog_info["epi_title"].toString();
            descr = prog_info["descr"].toString();
            syn_epi_num = prog_info["syn_epi_num"].toString();
            orig_air_date = prog_info["orig_air_date"].toString();

            MSqlQuery query(MSqlQuery::InitCon());
            query.prepare(
                "SELECT chanid FROM channel WHERE xmltvid = :XMLID"
            );

            query.bindValue(":XMLID", filename.left(5));

            if (!query.exec())
            {
                MythDB::DBError("FillData::grabData", query);
                return false;
            }

            int chanid;

            if (query.next())
                // We're going to update all chanid's in the database that use this particular XMLID.
            {

                chanid = query.value(0).toInt();

                // Temp values
                bool is_subtitled = false;


                // Living dangerously, or speeding things up?
                // Sanity check on whether the downloaded data is valid first?

                MSqlQuery purge(MSqlQuery::InitCon());
                purge.prepare(
                    "DELETE FROM program where chanid = :CHANID"
                );

                purge.bindValue(":CHANID", chanid);

                if (!purge.exec())
                {
                    MythDB::DBError("Deleting data", purge);
                    return false;
                }

                MSqlQuery insert(MSqlQuery::InitCon());
                insert.prepare(
                    "INSERT INTO program ("
                    "chanid, starttime, endtime,"
                    "title, subtitle, description,"
                    "stereo, subtitled, hdtv,"
                    "closecaptioned, partnumber, parttotal,"
                    "seriesid, originalairdate, programid) "
                    "VALUES ("
                    ":CHANID, :STARTTIME, :ENDTIME,"
                    ":TITLE, :SUBTITLE, :DESCRIPTION,"
                    ":STEREO, :SUBTITLED, :HDTV,"
                    ":CLOSECAPTIONED, :PARTNUMBER, :PARTTOTAL,"
                    ":SERIESID, :ORIGINALAIRDATE, :PROGID)");

                insert.bindValue(":CHANID", chanid);
                insert.bindValue(":STARTTIME", UTCdt_start);
                insert.bindValue(":ENDTIME", UTCdt_end);
                insert.bindValue(":TITLE", title);
                insert.bindValue(":SUBTITLE", epi_title);
                insert.bindValue(":DESCRIPTION", descr);
                insert.bindValue(":STEREO", is_stereo);
                insert.bindValue(":SUBTITLED", is_subtitled);
                insert.bindValue(":HDTV", is_hdtv);
                insert.bindValue(":CLOSECAPTIONED", is_cc);
                insert.bindValue(":PARTNUMBER", part_num);
                insert.bindValue(":PARTTOTAL", num_parts);
                insert.bindValue(":SERIESID", syn_epi_num);
                insert.bindValue(":ORIGINALAIRDATE", orig_air_date);
                insert.bindValue(":PROGID", i.key());

                if (!insert.exec())
                {
                    MythDB::DBError("Loading data", insert);
                    return false;
                }
            }
        } // end of the while loop
    } // end of iterating through all the files.

    updateLastRunEnd(startstopstatus_query);

    status = "Completed download and update of database. Success.";
    updateLastRunStatus(startstopstatus_query, status);

    return true;

    //    return GrabData(s, offset, currentd);
}


/** \fn FillData::Run(SourceList &sourcelist)
 *  \brief Goes through the sourcelist and updates its channels with
 *         program info grabbed with the associated grabber.
 *  \return true if there were no failures
 */
bool FillData::Run(SourceList &sourcelist)
{
    SourceList::iterator it;
    SourceList::iterator it2;

    QString status, querystr;
    MSqlQuery query(MSqlQuery::InitCon());
    QDateTime GuideDataBefore, GuideDataAfter;
    int failures = 0;
    int externally_handled = 0;
    int total_sources = sourcelist.size();
    int source_channels = 0;

    QString sidStr = QString("Updating source #%1 (%2) with grabber %3");

    need_post_grab_proc = false;
    int nonewdata = 0;
    bool has_dd_source = false;

    // find all DataDirect duplicates, so we only data download once.
    for (it = sourcelist.begin(); it != sourcelist.end(); ++it)
    {
        if (!is_grabber_datadirect((*it).xmltvgrabber))
            continue;

        has_dd_source = true;

        for (it2 = sourcelist.begin(); it2 != sourcelist.end(); ++it2)
        {
            if (((*it).id           != (*it2).id)           &&
                ((*it).xmltvgrabber == (*it2).xmltvgrabber) &&
                ((*it).userid       == (*it2).userid)       &&
                ((*it).password     == (*it2).password))
            {
                (*it).dd_dups.push_back((*it2).id);
            }
        }
    }

    if (has_dd_source)
        ddprocessor.CreateTempDirectory();

    for (it = sourcelist.begin(); it != sourcelist.end(); ++it)
    {

        if (!fatalErrors.empty())
            break;

        query.prepare("SELECT MAX(endtime) FROM program p LEFT JOIN channel c "
                      "ON p.chanid=c.chanid WHERE c.sourceid= :SRCID "
                      "AND manualid = 0 AND c.xmltvid != '';");
        query.bindValue(":SRCID", (*it).id);

        if (query.exec() && query.next())
        {
            if (!query.isNull(0))
                GuideDataBefore =
                    MythDate::fromString(query.value(0).toString());
        }

        channel_update_run = false;
        endofdata = false;

        QString xmltv_grabber = (*it).xmltvgrabber;

        if (xmltv_grabber == "eitonly")
        {
            LOG(VB_GENERAL, LOG_INFO,
                QString("Source %1 configured to use only the "
                        "broadcasted guide data. Skipping.") .arg((*it).id));

            externally_handled++;
            updateLastRunStart(query);
            updateLastRunEnd(query);
            continue;
        }
        else if (xmltv_grabber.trimmed().isEmpty() ||
                 xmltv_grabber == "/bin/true" ||
                 xmltv_grabber == "none")
        {
            LOG(VB_GENERAL, LOG_INFO,
                QString("Source %1 configured with no grabber. Nothing to do.")
                .arg((*it).id));

            externally_handled++;
            updateLastRunStart(query);
            updateLastRunEnd(query);
            continue;
        }

        LOG(VB_GENERAL, LOG_INFO, sidStr.arg((*it).id)
            .arg((*it).name)
            .arg(xmltv_grabber));

        query.prepare(
            "SELECT COUNT(chanid) FROM channel WHERE sourceid = "
            ":SRCID AND xmltvid != ''");
        query.bindValue(":SRCID", (*it).id);

        if (query.exec() && query.next())
        {
            source_channels = query.value(0).toInt();

            if (source_channels > 0)
            {
                LOG(VB_GENERAL, LOG_INFO,
                    QString("Found %1 channels for source %2 which use grabber")
                    .arg(source_channels).arg((*it).id));
            }
            else
            {
                LOG(VB_GENERAL, LOG_INFO,
                    QString("No channels are configured to use grabber."));
            }
        }
        else
        {
            source_channels = 0;
            LOG(VB_GENERAL, LOG_INFO,
                QString("Can't get a channel count for source id %1")
                .arg((*it).id));
        }

        bool hasprefmethod = false;

        if (is_grabber_external(xmltv_grabber))
        {
            uint flags = kMSRunShell | kMSStdOut | kMSBuffered;
            MythSystem grabber_capabilities_proc(xmltv_grabber,
                                                 QStringList("--capabilities"),
                                                 flags);
            grabber_capabilities_proc.Run(25);

            if (grabber_capabilities_proc.Wait() != GENERIC_EXIT_OK)
                LOG(VB_GENERAL, LOG_ERR,
                    QString("%1  --capabilities failed or we timed out waiting."
                            " You may need to upgrade your xmltv grabber")
                    .arg(xmltv_grabber));
            else
            {
                QByteArray result = grabber_capabilities_proc.ReadAll();
                QTextStream ostream(result);
                QString capabilities;

                while (!ostream.atEnd())
                {
                    QString capability
                    = ostream.readLine().simplified();

                    if (capability.isEmpty())
                        continue;

                    capabilities += capability + ' ';

                    if (capability == "baseline")
                        (*it).xmltvgrabber_baseline = true;

                    if (capability == "manualconfig")
                        (*it).xmltvgrabber_manualconfig = true;

                    if (capability == "cache")
                        (*it).xmltvgrabber_cache = true;

                    if (capability == "preferredmethod")
                        hasprefmethod = true;
                }

                LOG(VB_GENERAL, LOG_INFO,
                    QString("Grabber has capabilities: %1") .arg(capabilities));
            }
        }

        if (hasprefmethod)
        {
            uint flags = kMSRunShell | kMSStdOut | kMSBuffered;
            MythSystem grabber_method_proc(xmltv_grabber,
                                           QStringList("--preferredmethod"),
                                           flags);
            grabber_method_proc.Run(15);

            if (grabber_method_proc.Wait() != GENERIC_EXIT_OK)
                LOG(VB_GENERAL, LOG_ERR,
                    QString("%1 --preferredmethod failed or we timed out "
                            "waiting. You may need to upgrade your xmltv "
                            "grabber").arg(xmltv_grabber));
            else
            {
                QTextStream ostream(grabber_method_proc.ReadAll());
                (*it).xmltvgrabber_prefmethod =
                    ostream.readLine().simplified();

                LOG(VB_GENERAL, LOG_INFO, QString("Grabber prefers method: %1")
                    .arg((*it).xmltvgrabber_prefmethod));
            }
        }

        need_post_grab_proc |= !is_grabber_datadirect(xmltv_grabber);

        if (xmltv_grabber == "schedulesdirect2")
        {
            /*
            * The "schedulesdirect1" grabber is for the internal grabber to TMS
            * so we use schedulesdirect2 to differentiate.
            * Process for downloading Schedules Direct JSON data files.
            * Execute a login to http://rkulagow.schedulesdirect.org/login.php
            * Scan the downloaded file for the randhash.
            * Download status messages
            * Check the version number of the headend
            *
            */
            QString randhash = GetSDLoginRandhash(*it);

            //qDebug() << "randhash is " << randhash;
            if (randhash == "error")
            {
                qDebug() << "Error getting randhash.";
                exit;
            }

            if (getSchedulesDirectStatusMessages(randhash))
            {
                qDebug() << "Status message from Schedules Direct";
            }

            if (is_SDHeadendVersionUpdated(*it))
            {
                qDebug() << "Headend updated. Do something here; write a message to the log.";
            }


            if (!DownloadSDFiles(randhash))
            {
                qDebug() << "Error downloading files.";
            }

        } // Done with the schedulesdirect stuff.

        if (is_grabber_datadirect(xmltv_grabber) && dd_grab_all)
        {
            if (only_update_channels)
                DataDirectUpdateChannels(*it);
            else
            {
                QDate qCurrentDate = MythDate::current().date();

                if (!GrabData(*it, 0, &qCurrentDate))
                    ++failures;
            }
        }
        else if ((*it).xmltvgrabber_prefmethod == "allatonce")
        {
            if (!GrabData(*it, 0))
                ++failures;
        }
        else if ((*it).xmltvgrabber_baseline ||
                 is_grabber_datadirect(xmltv_grabber))
        {

            QDate qCurrentDate = MythDate::current().date();

            // We'll keep grabbing until it returns nothing
            // Max days currently supported is 21
            int grabdays = (is_grabber_datadirect(xmltv_grabber)) ?
                           14 : REFRESH_MAX;

            grabdays = (maxDays > 0)          ? maxDays : grabdays;
            grabdays = (only_update_channels) ? 1       : grabdays;

            vector<bool> refresh_request;
            refresh_request.resize(grabdays, refresh_all);

            for (int i = 0; i < refresh_day.size(); i++)
                refresh_request[i] = refresh_day[i];

            if (is_grabber_datadirect(xmltv_grabber) && only_update_channels)
            {
                DataDirectUpdateChannels(*it);
                grabdays = 0;
            }

            for (int i = 0; i < grabdays; i++)
            {
                if (!fatalErrors.empty())
                    break;

                // We need to check and see if the current date has changed
                // since we started in this loop.  If it has, we need to adjust
                // the value of 'i' to compensate for this.
                if (MythDate::current().date() != qCurrentDate)
                {
                    QDate newDate = MythDate::current().date();
                    i += (newDate.daysTo(qCurrentDate));

                    if (i < 0)
                        i = 0;

                    qCurrentDate = newDate;
                }

                QString prevDate(qCurrentDate.addDays(i - 1).toString());
                QString currDate(qCurrentDate.addDays(i).toString());

                LOG(VB_GENERAL, LOG_INFO, ""); // add a space between days
                LOG(VB_GENERAL, LOG_INFO, "Checking day @ " +
                    QString("offset %1, date: %2").arg(i).arg(currDate));

                bool download_needed = false;

                if (refresh_request[i])
                {
                    if (i == 1)
                    {
                        LOG(VB_GENERAL, LOG_INFO,
                            "Data Refresh always needed for tomorrow");
                    }
                    else
                    {
                        LOG(VB_GENERAL, LOG_INFO,
                            "Data Refresh needed because of user request");
                    }

                    download_needed = true;
                }
                else
                {
                    // Check to see if we already downloaded data for this date.

                    querystr = "SELECT c.chanid, COUNT(p.starttime) "
                               "FROM channel c "
                               "LEFT JOIN program p ON c.chanid = p.chanid "
                               "  AND starttime >= "
                               "DATE_ADD(DATE_ADD(CURRENT_DATE(), "
                               "INTERVAL '%1' DAY), INTERVAL '20' HOUR) "
                               "  AND starttime < DATE_ADD(CURRENT_DATE(), "
                               "INTERVAL '%2' DAY) "
                               "WHERE c.sourceid = %3 AND c.xmltvid != '' "
                               "GROUP BY c.chanid;";

                    if (query.exec(querystr.arg(i - 1).arg(i).arg((*it).id)) &&
                        query.isActive())
                    {
                        int prevChanCount = 0;
                        int currentChanCount = 0;
                        int previousDayCount = 0;
                        int currentDayCount = 0;

                        LOG(VB_CHANNEL, LOG_INFO,
                            QString("Checking program counts for day %1")
                            .arg(i - 1));

                        while (query.next())
                        {
                            if (query.value(1).toInt() > 0)
                                prevChanCount++;

                            previousDayCount += query.value(1).toInt();

                            LOG(VB_CHANNEL, LOG_INFO,
                                QString("    chanid %1 -> %2 programs")
                                .arg(query.value(0).toString())
                                .arg(query.value(1).toInt()));
                        }

                        if (query.exec(querystr.arg(i).arg(i + 1).arg((*it).id))
                            && query.isActive())
                        {
                            LOG(VB_CHANNEL, LOG_INFO,
                                QString("Checking program counts for day %1")
                                .arg(i));

                            while (query.next())
                            {
                                if (query.value(1).toInt() > 0)
                                    currentChanCount++;

                                currentDayCount += query.value(1).toInt();

                                LOG(VB_CHANNEL, LOG_INFO,
                                    QString("    chanid %1 -> %2 programs")
                                    .arg(query.value(0).toString())
                                    .arg(query.value(1).toInt()));
                            }
                        }
                        else
                        {
                            LOG(VB_GENERAL, LOG_INFO,
                                QString("Data Refresh because we are unable to "
                                        "query the data for day %1 to "
                                        "determine if we have enough").arg(i));
                            download_needed = true;
                        }

                        if (currentChanCount < (prevChanCount * 0.90))
                        {
                            LOG(VB_GENERAL, LOG_INFO,
                                QString("Data refresh needed because only %1 "
                                        "out of %2 channels have at least one "
                                        "program listed for day @ offset %3 "
                                        "from 8PM - midnight.  Previous day "
                                        "had %4 channels with data in that "
                                        "time period.")
                                .arg(currentChanCount).arg(source_channels)
                                .arg(i).arg(prevChanCount));
                            download_needed = true;
                        }
                        else if (currentDayCount == 0)
                        {
                            LOG(VB_GENERAL, LOG_INFO,
                                QString("Data refresh needed because no data "
                                        "exists for day @ offset %1 from 8PM - "
                                        "midnight.").arg(i));
                            download_needed = true;
                        }
                        else if (previousDayCount == 0)
                        {
                            LOG(VB_GENERAL, LOG_INFO,
                                QString("Data refresh needed because no data "
                                        "exists for day @ offset %1 from 8PM - "
                                        "midnight.  Unable to calculate how "
                                        "much we should have for the current "
                                        "day so a refresh is being forced.")
                                .arg(i - 1));
                            download_needed = true;
                        }
                        else if (currentDayCount < (currentChanCount * 3))
                        {
                            LOG(VB_GENERAL, LOG_INFO,
                                QString("Data Refresh needed because offset "
                                        "day %1 has less than 3 programs "
                                        "per channel for the 8PM - midnight "
                                        "time window for channels that "
                                        "normally have data. "
                                        "We want at least %2 programs, but "
                                        "only found %3")
                                .arg(i).arg(currentChanCount * 3)
                                .arg(currentDayCount));
                            download_needed = true;
                        }
                        else if (currentDayCount < (previousDayCount / 2))
                        {
                            LOG(VB_GENERAL, LOG_INFO,
                                QString("Data Refresh needed because offset "
                                        "day %1 has less than half the number "
                                        "of programs as the previous day for "
                                        "the 8PM - midnight time window. "
                                        "We want at least %2 programs, but "
                                        "only found %3").arg(i)
                                .arg(previousDayCount / 2)
                                .arg(currentDayCount));
                            download_needed = true;
                        }
                    }
                    else
                    {
                        LOG(VB_GENERAL, LOG_INFO,
                            QString("Data Refresh needed because we are unable "
                                    "to query the data for day @ offset %1 to "
                                    "determine how much we should have for "
                                    "offset day %2.").arg(i - 1).arg(i));
                        download_needed = true;
                    }
                }

                if (download_needed)
                {
                    LOG(VB_GENERAL, LOG_NOTICE,
                        QString("Refreshing data for ") + currDate);

                    if (!GrabData(*it, i, &qCurrentDate))
                    {
                        ++failures;

                        if (!fatalErrors.empty() || interrupted)
                        {
                            break;
                        }
                    }

                    if (endofdata)
                    {
                        LOG(VB_GENERAL, LOG_INFO,
                            "Grabber is no longer returning program data, "
                            "finishing");
                        break;
                    }
                }
                else
                {
                    LOG(VB_GENERAL, LOG_NOTICE,
                        QString("Data is already present for ") + currDate +
                        ", skipping");
                }
            }

            if (!fatalErrors.empty())
                break;
        }
        else
        {
            if (xmltv_grabber != "schedulesdirect2")
            {
                // only print an error if we're not using schedulesdirect2
                LOG(VB_GENERAL, LOG_ERR,
                    QString("Grabbing XMLTV data using ") + xmltv_grabber +
                    " is not supported. You may need to upgrade to"
                    " the latest version of XMLTV.");
            }
        }

        if (interrupted)
        {
            break;
        }

        query.prepare("SELECT MAX(endtime) FROM program p LEFT JOIN channel c "
                      "ON p.chanid=c.chanid WHERE c.sourceid= :SRCID "
                      "AND manualid = 0 AND c.xmltvid != '';");
        query.bindValue(":SRCID", (*it).id);

        if (query.exec() && query.next())
        {
            if (!query.isNull(0))
                GuideDataAfter = MythDate::fromString(query.value(0).toString());
        }

        if (GuideDataAfter == GuideDataBefore)
        {
            nonewdata++;
        }
    }

    if (!fatalErrors.empty())
    {
        for (int i = 0; i < fatalErrors.size(); i++)
        {
            LOG(VB_GENERAL, LOG_CRIT, LOC + "Encountered Fatal Error: " +
                fatalErrors[i]);
        }

        return false;
    }

    if (only_update_channels && !need_post_grab_proc)
        return true;

    if (failures == 0)
    {
        if (nonewdata > 0 &&
            (total_sources != externally_handled))
            status = QString(QObject::tr(
                                 "mythfilldatabase ran, but did not insert "
                                 "any new data into the Guide for %1 of %2 sources. "
                                 "This can indicate a potential grabber failure."))
                     .arg(nonewdata)
                     .arg(total_sources);
        else
            status = QObject::tr("Successful.");

        updateLastRunStatus(query, status);
    }

    return (failures == 0);
}

ChanInfo *FillData::xawtvChannel(QString &id, QString &channel, QString &fine)
{
    ChanInfo *chaninfo = new ChanInfo;
    chaninfo->xmltvid = id;
    chaninfo->name = id;
    chaninfo->callsign = id;

    if (chan_data.channel_preset)
        chaninfo->chanstr = id;
    else
        chaninfo->chanstr = channel;

    chaninfo->finetune = fine;
    chaninfo->freqid = channel;
    chaninfo->tvformat = "Default";

    return chaninfo;
}

void FillData::readXawtvChannels(int id, QString xawrcfile)
{
    QByteArray tmp = xawrcfile.toAscii();
    fstream fin(tmp.constData(), ios::in);

    if (!fin.is_open())
        return;

    QList<ChanInfo> chanlist;

    QString xawid;
    QString channel;
    QString fine;

    string strLine;
    int nSplitPoint = 0;

    while (!fin.eof())
    {
        getline(fin, strLine);

        if ((strLine[0] != '#') && (!strLine.empty()))
        {
            if (strLine[0] == '[')
            {
                if ((nSplitPoint = strLine.find(']')) > 1)
                {
                    if (!xawid.isEmpty() && !channel.isEmpty())
                    {
                        ChanInfo *chinfo = xawtvChannel(xawid, channel, fine);
                        chanlist.push_back(*chinfo);
                        delete chinfo;
                    }

                    xawid = strLine.substr(1, nSplitPoint - 1).c_str();
                    channel.clear();
                    fine.clear();
                }
            }
            else if ((nSplitPoint = strLine.find('=') + 1) > 0)
            {
                while (strLine.substr(nSplitPoint, 1) == " ")
                {
                    ++nSplitPoint;
                }

                if (!strncmp(strLine.c_str(), "channel", 7))
                {
                    channel = strLine.substr(nSplitPoint,
                                             strLine.size()).c_str();
                }
                else if (!strncmp(strLine.c_str(), "fine", 4))
                {
                    fine = strLine.substr(nSplitPoint, strLine.size()).c_str();
                }
            }
        }
    }

    if (!xawid.isEmpty() && !channel.isEmpty())
    {
        ChanInfo *chinfo = xawtvChannel(xawid, channel, fine);
        chanlist.push_back(*chinfo);
        delete chinfo;
    }

    chan_data.handleChannels(id, &chanlist);
    icon_data.UpdateSourceIcons(id);
}

/* vim: set expandtab tabstop=4 shiftwidth=4: */
