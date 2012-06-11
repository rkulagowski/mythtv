#include <unistd.h>
#include <zlib.h>

// Qt headers
#include <QFile>
#include <QDir>

// libmyth headers
#include "mythdirs.h"
#include "mythlogging.h"

// filldata headers
#include "fillutil.h"

bool dash_open(QFile &file, const QString &filename, int m, FILE *handle)
{
    bool retval = false;
    if (filename == "-")
    {
        if (handle == NULL)
        {
            handle = stdout;
            if (m & QIODevice::ReadOnly)
            {
                handle = stdin;
            }
        }
        retval = file.open( handle, (QIODevice::OpenMode)m );
    }
    else
    {
        file.setFileName(filename);
        retval = file.open( (QIODevice::OpenMode)m );
    }

    return retval;
}

QString SetupIconCacheDirectory(void)
{
    QString fileprefix = GetConfDir();

    QDir dir(fileprefix);
    if (!dir.exists())
        dir.mkdir(fileprefix);

    fileprefix += "/channels";

    dir = QDir(fileprefix);
    if (!dir.exists())
        dir.mkdir(fileprefix);

    return fileprefix;
}


/*
 * This function taken from: 
 * http://stackoverflow.com/questions/2690328/qt-quncompress-gzip-data
 *
 * Based on zlib example code.
 *
 * Copyright (c) 2011 Ralf Engels <ralf-engels@gmx.de>
 * Copyright (C) 1995-2012 Jean-loup Gailly and Mark Adler
 *
 * Licensed under the terms of the ZLib license which is found at
 * http://zlib.net/zlib_license.html and is as follows:
 *
 *  This software is provided 'as-is', without any express or implied
 *  warranty.  In no event will the authors be held liable for any damages
 *  arising from the use of this software.
 *
 *  Permission is granted to anyone to use this software for any purpose,
 *  including commercial applications, and to alter it and redistribute it
 *  freely, subject to the following restrictions:
 *
 *  1. The origin of this software must not be misrepresented; you must not
 *     claim that you wrote the original software. If you use this software
 *     in a product, an acknowledgment in the product documentation would be
 *     appreciated but is not required.
 *  2. Altered source versions must be plainly marked as such, and must not be
 *     misrepresented as being the original software.
 *  3. This notice may not be removed or altered from any source distribution.
 *
 *  NOTE: The Zlib license is listed as being GPL-compatible
 *    http://www.gnu.org/licenses/license-list.html#ZLib
 */

QByteArray gUncompress(const QByteArray &data)
{
    if (data.size() <= 4) {
        LOG(VB_GENERAL, LOG_WARNING, "gUncompress: Input data is truncated");
        return QByteArray();
    }

    QByteArray result;

    int ret;
    z_stream strm;
    static const int CHUNK_SIZE = 1024;
    char out[CHUNK_SIZE];

    /* allocate inflate state */
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = data.size();
    strm.next_in = (Bytef*)(data.data());

    ret = inflateInit2(&strm, 15 +  32); // gzip decoding
    if (ret != Z_OK)
        return QByteArray();

    // run inflate()
    do {
        strm.avail_out = CHUNK_SIZE;
        strm.next_out = (Bytef*)(out);

        ret = inflate(&strm, Z_NO_FLUSH);
        Q_ASSERT(ret != Z_STREAM_ERROR);  // state not clobbered

        switch (ret) {
        case Z_NEED_DICT:
            ret = Z_DATA_ERROR;     // and fall through
        case Z_DATA_ERROR:
        case Z_MEM_ERROR:
            (void)inflateEnd(&strm);
            return QByteArray();
        }

        result.append(out, CHUNK_SIZE - strm.avail_out);
    } while (strm.avail_out == 0);

    // clean up and return
    inflateEnd(&strm);
    return result;
}
