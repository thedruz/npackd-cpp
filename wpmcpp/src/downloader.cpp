#include <math.h>
#include <stdint.h>

#include <windows.h>
#include <wininet.h>

#include <zlib.h>

#include <QObject>
#include <QDebug>
#include <QWaitCondition>
#include <QMutex>
#include <QCryptographicHash>
#include <QProcess>

#include "downloader.h"
#include "job.h"
#include "wpmutils.h"

HWND defaultPasswordWindow = 0;
QMutex loginDialogMutex;

DWORD __stdcall myInternetAuthNotifyCallback(DWORD_PTR dwContext,
        DWORD dwReturn, LPVOID lpReserved) {
    qDebug() << "myInternetAuthNotifyCallback" << dwReturn;
    return 0;
}

int64_t Downloader::downloadWin(Job* job, const QUrl& url, LPCWSTR verb,
        QFile* file,
        QString* mime, QString* contentDisposition,
        HWND parentWindow, QString* sha1, bool useCache,
        QCryptographicHash::Algorithm alg, bool keepConnection, int timeout,
        bool interactive)
{
    QString initialTitle = job->getTitle();

    job->setTitle(initialTitle + " / " + QObject::tr("Connecting"));

    if (sha1)
        sha1->clear();

    QString server = url.host();
    QString resource = url.path();
    QString encQuery = url.query(QUrl::FullyEncoded);
    if (!encQuery.isEmpty())
        resource.append('?').append(encQuery);

    QString agent("Npackd/");
    agent.append(NPACKD_VERSION);

    agent += " (compatible; MSIE 9.0)";

    HINTERNET internet = InternetOpenW((WCHAR*) agent.utf16(),
            INTERNET_OPEN_TYPE_PRECONFIG,
            0, 0, 0);

    if (internet == 0) {
        QString errMsg;
        WPMUtils::formatMessage(GetLastError(), &errMsg);
        job->setErrorMessage(errMsg);
    }

    // change the timeout to 5 minutes
    if (job->shouldProceed()) {
        DWORD rec_timeout = timeout * 1000;
        InternetSetOption(internet, INTERNET_OPTION_RECEIVE_TIMEOUT,
                &rec_timeout, sizeof(rec_timeout));
        job->setProgress(0.01);
    }

    HINTERNET hConnectHandle = 0;
    if (job->shouldProceed()) {
        INTERNET_PORT port = url.port(url.scheme() == "https" ?
                INTERNET_DEFAULT_HTTPS_PORT: INTERNET_DEFAULT_HTTP_PORT);
        hConnectHandle = InternetConnectW(internet,
                (WCHAR*) server.utf16(), port, 0, 0, INTERNET_SERVICE_HTTP, 0, 0);

        if (hConnectHandle == 0) {
            QString errMsg;
            WPMUtils::formatMessage(GetLastError(), &errMsg);
            job->setErrorMessage(errMsg);
        }
    }


    // flags: http://msdn.microsoft.com/en-us/library/aa383661(v=vs.85).aspx
    // We support accepting any mime file type since this is a simple download
    // of a file
    HINTERNET hResourceHandle = 0;
    if (job->shouldProceed()) {
        LPCTSTR ppszAcceptTypes[2];
        ppszAcceptTypes[0] = L"*/*";
        ppszAcceptTypes[1] = NULL;
        DWORD flags = (url.scheme() == "https" ? INTERNET_FLAG_SECURE : 0);
        if (keepConnection)
            flags |= INTERNET_FLAG_KEEP_CONNECTION;
        flags |= INTERNET_FLAG_RESYNCHRONIZE;
        if (!useCache)
            flags |= INTERNET_FLAG_DONT_CACHE | INTERNET_FLAG_PRAGMA_NOCACHE |
                    INTERNET_FLAG_RELOAD;
        hResourceHandle = HttpOpenRequestW(hConnectHandle, verb,
                (WCHAR*) resource.utf16(),
                0, 0, ppszAcceptTypes,
                flags, 0);
        if (hResourceHandle == 0) {
            QString errMsg;
            WPMUtils::formatMessage(GetLastError(), &errMsg);
            job->setErrorMessage(errMsg);
        }
    }

    if (job->shouldProceed()) {
        job->checkOSCall(HttpAddRequestHeadersW(hResourceHandle,
                L"Accept-Encoding: gzip, deflate", -1,
                HTTP_ADDREQ_FLAG_ADD));
    }

    DWORD dwStatus, dwStatusSize = sizeof(dwStatus);

    // qDebug() << "download.5";
    int retries = 0;
    while (job->shouldProceed()) {
        retries++;
        if (retries >= 5) {
            job->setErrorMessage(
                    QObject::tr("Too many retries during the download"));
            break;
        }

        // qDebug() << "download.5.1";

        DWORD sendRequestError = 0;
        if (!HttpSendRequestW(hResourceHandle, 0, 0, 0, 0)) {
            sendRequestError = GetLastError();
        }

        // http://msdn.microsoft.com/en-us/library/aa384220(v=vs.85).aspx
        if (!HttpQueryInfo(hResourceHandle, HTTP_QUERY_FLAG_NUMBER |
                HTTP_QUERY_STATUS_CODE, &dwStatus, &dwStatusSize, NULL)) {
            QString errMsg;
            WPMUtils::formatMessage(GetLastError(), &errMsg);
            job->setErrorMessage(errMsg);
            break;
        }

        // 2XX
        if (sendRequestError == 0) {
            if (dwStatus / 100 == 2)
                break;
        }

        if (parentWindow) {
            INTERNET_AUTH_NOTIFY_DATA nd = {};
            nd.cbStruct = sizeof(nd);
            nd.pfnNotify = &myInternetAuthNotifyCallback;

            void* p = &nd;

            DWORD flags = FLAGS_ERROR_UI_FILTER_FOR_ERRORS |
                          FLAGS_ERROR_UI_FLAGS_CHANGE_OPTIONS |
                          FLAGS_ERROR_UI_FLAGS_GENERATE_DATA; // |
                          // FLAGS_ERROR_UI_SERIALIZE_DIALOGS;

            // both calls to InternetErrorDlg should be enclosed by one
            // mutex, so that only one dialog will be shown
            loginDialogMutex.lock();
            DWORD r;
            //do {
                r = InternetErrorDlg(parentWindow,
                        hResourceHandle, sendRequestError,
                        flags | FLAGS_ERROR_UI_FLAGS_NO_UI, &p);
            //} while (r == ERROR_INTERNET_DIALOG_PENDING);

            //qDebug() << "111:" << r;
            if (r == ERROR_SUCCESS) {
                if (interactive) {
                    r = InternetErrorDlg(parentWindow,
                            hResourceHandle, sendRequestError, flags, &p);

                    //qDebug() << "222:" << r;
                    if (r == ERROR_SUCCESS) {
                        // retry
                    } else if (r == ERROR_INTERNET_FORCE_RETRY) {
                        // the call above set the password and we should retry the
                        // request
                    } else if (r == ERROR_CANCELLED) {
                        job->setErrorMessage(QObject::tr("Cancelled by the user"));
                    } else if (r == ERROR_INVALID_HANDLE) {
                        job->setErrorMessage(QObject::tr("Invalid handle"));
                    } else {
                        job->setErrorMessage(QString(
                                QObject::tr("Unknown error %1 from InternetErrorDlg")).arg(r));
                    }
                } else {
                    // retry
                }
            } else if (r == ERROR_INTERNET_FORCE_RETRY) {
                // nothing
            } else if (r == ERROR_CANCELLED) {
                job->setErrorMessage(QObject::tr("Cancelled by the user"));
            } else if (r == ERROR_INVALID_HANDLE) {
                job->setErrorMessage(QObject::tr("Invalid handle"));
            } else {
                job->setErrorMessage(QString(
                        QObject::tr("Unknown error %1 from InternetErrorDlg")).arg(r));
            }
            loginDialogMutex.unlock();
        } else {
            void* p = 0;

            DWORD flags = FLAGS_ERROR_UI_FILTER_FOR_ERRORS |
                          FLAGS_ERROR_UI_FLAGS_CHANGE_OPTIONS |
                          FLAGS_ERROR_UI_FLAGS_GENERATE_DATA |
                          FLAGS_ERROR_UI_FLAGS_NO_UI;
            DWORD r = InternetErrorDlg(0,
                    hResourceHandle, sendRequestError, flags, &p);
            //qDebug() << "333" << r;
            if (r == ERROR_SUCCESS) {
                if (interactive) {
                    loginDialogMutex.lock();
                    QString e = inputPassword(hConnectHandle, dwStatus); // nothing
                    loginDialogMutex.unlock();
                    //qDebug() << "inputPassword: " << e;
                    if (!e.isEmpty()) {
                        job->setErrorMessage(e);
                    }
                } else {
                    // retry
                }
            } else if (r == ERROR_INTERNET_FORCE_RETRY) {
                // the call above set the password and we should retry the
                // request
            } else if (r == ERROR_CANCELLED) {
                job->setErrorMessage(QObject::tr("Cancelled by the user"));
            } else if (r == ERROR_INVALID_HANDLE) {
                job->setErrorMessage(QObject::tr("Invalid handle"));
            } else {
                job->setErrorMessage(QString(
                        QObject::tr("Unknown error %1 from InternetErrorDlg")).arg(r));
            }
        }

        if (!job->shouldProceed())
            break;

        // read all the data before re-sending the request
        char smallBuffer[4 * 1024];
        while (true) {
            DWORD read;
            if (!InternetReadFile(hResourceHandle, &smallBuffer,
                    sizeof(smallBuffer), &read)) {
                QString errMsg;
                WPMUtils::formatMessage(GetLastError(), &errMsg);
                job->setErrorMessage(errMsg);
                goto out;
            }

            // qDebug() << "read some bytes " << read;
            if (read == 0)
                break;
        }
    }; // while (job->shouldProceed())

out:
    if (job->shouldProceed()) {
        // http://msdn.microsoft.com/en-us/library/aa384220(v=vs.85).aspx
        if (!HttpQueryInfo(hResourceHandle, HTTP_QUERY_FLAG_NUMBER |
                HTTP_QUERY_STATUS_CODE, &dwStatus, &dwStatusSize, NULL)) {
            QString errMsg;
            WPMUtils::formatMessage(GetLastError(), &errMsg);
            job->setErrorMessage(errMsg);
        } else {
            // 2XX
            if (dwStatus / 100 != 2) {
                job->setErrorMessage(QString(
                        QObject::tr("HTTP status code %1")).arg(dwStatus));
            }
        }
    }

    if (job->shouldProceed()) {
        job->setProgress(0.03);
        job->setTitle(initialTitle + " / " + QObject::tr("Downloading"));
    }

    // MIME type
    if (job->shouldProceed()) {
        if (mime) {
            WCHAR mimeBuffer[1024];
            DWORD bufferLength = sizeof(mimeBuffer);
            DWORD index = 0;
            if (!HttpQueryInfoW(hResourceHandle, HTTP_QUERY_CONTENT_TYPE,
                    &mimeBuffer, &bufferLength, &index)) {
                QString errMsg;
                WPMUtils::formatMessage(GetLastError(), &errMsg);
                job->setErrorMessage(errMsg);
            } else {
                mime->setUtf16((ushort*) mimeBuffer, bufferLength / 2);
            }
        }
    }

    bool gzip = false;

    // Content-Encoding
    if (job->shouldProceed()) {
        WCHAR contentEncodingBuffer[1024];
        DWORD bufferLength = sizeof(contentEncodingBuffer);
        DWORD index = 0;
        if (HttpQueryInfoW(hResourceHandle, HTTP_QUERY_CONTENT_ENCODING,
                &contentEncodingBuffer, &bufferLength, &index)) {
            QString contentEncoding;
            contentEncoding.setUtf16((ushort*) contentEncodingBuffer,
                    bufferLength / 2);
            gzip = contentEncoding == "gzip" || contentEncoding == "deflate";
        }

        job->setProgress(0.04);
    }

    // Content-Disposition
    if (job->shouldProceed()) {
        if (contentDisposition) {
            WCHAR cdBuffer[1024];
            wcscpy(cdBuffer, L"Content-Disposition");
            DWORD bufferLength = sizeof(cdBuffer);
            DWORD index = 0;
            if (HttpQueryInfoW(hResourceHandle, HTTP_QUERY_CUSTOM,
                    &cdBuffer, &bufferLength, &index)) {
                contentDisposition->setUtf16((ushort*) cdBuffer,
                        bufferLength / 2);
            }
        }
    }

    int64_t contentLength = -1;

    // content length
    if (job->shouldProceed()) {
        WCHAR contentLengthBuffer[100];
        DWORD bufferLength = sizeof(contentLengthBuffer);
        DWORD index = 0;
        if (HttpQueryInfoW(hResourceHandle, HTTP_QUERY_CONTENT_LENGTH,
                contentLengthBuffer, &bufferLength, &index)) {
            QString s;
            s.setUtf16((ushort*) contentLengthBuffer, bufferLength / 2);
            bool ok;
            contentLength = s.toLongLong(&ok, 10);
            if (!ok)
                contentLength = 0;
        }

        job->setProgress(0.05);
    }

    if (job->shouldProceed()) {
        Job* sub = job->newSubJob(0.95, QObject::tr("Reading the data"));
        readData(sub, hResourceHandle, file, sha1, gzip, contentLength, alg);
        if (!sub->getErrorMessage().isEmpty())
            job->setErrorMessage(sub->getErrorMessage());
    }

    if (hResourceHandle)
        InternetCloseHandle(hResourceHandle);
    if (hConnectHandle)
        InternetCloseHandle(hConnectHandle);
    if (internet)
        InternetCloseHandle(internet);

    if (job->shouldProceed())
        job->setProgress(1);

    job->setTitle(initialTitle);

    job->complete();

    return contentLength;
}

QString Downloader::inputPassword(HINTERNET hConnectHandle, DWORD dwStatus)
{
    QString result;

    QString username, password;
    if (dwStatus == HTTP_STATUS_PROXY_AUTH_REQ) {
        WPMUtils::outputTextConsole("\n" +
                QObject::tr("The HTTP proxy requires authentication.") + "\n");
        WPMUtils::outputTextConsole(QObject::tr("Username") + ": ");
        username = WPMUtils::inputTextConsole();
        WPMUtils::outputTextConsole(QObject::tr("Password") + ": ");
        password = WPMUtils::inputPasswordConsole();

        if (!InternetSetOptionW(hConnectHandle,
                INTERNET_OPTION_PROXY_USERNAME,
                (void*) username.utf16(),
                username.length() + 1)) {
            WPMUtils::formatMessage(GetLastError(), &result);
        }

        if (result.isEmpty() && !InternetSetOptionW(hConnectHandle,
                INTERNET_OPTION_PROXY_PASSWORD,
                (void*) password.utf16(),
                password.length() + 1)) {
            WPMUtils::formatMessage(GetLastError(), &result);
        }
    } else if (dwStatus == HTTP_STATUS_DENIED) {
        WPMUtils::outputTextConsole("\n" +
                QObject::tr("The HTTP server requires authentication.") +
                "\n");
        WPMUtils::outputTextConsole(QObject::tr("Username") + ": ");
        username = WPMUtils::inputTextConsole();
        WPMUtils::outputTextConsole(QObject::tr("Password") + ": ");
        password = WPMUtils::inputPasswordConsole();

        if (!InternetSetOptionW(hConnectHandle,
                INTERNET_OPTION_USERNAME,
                (void*) username.utf16(),
                username.length() + 1)) {
            WPMUtils::formatMessage(GetLastError(), &result);
        }

        if (result.isEmpty() && !InternetSetOptionW(hConnectHandle,
                INTERNET_OPTION_PASSWORD,
                (void*) password.utf16(),
                password.length() + 1)) {
            WPMUtils::formatMessage(GetLastError(), &result);
        }
    } else {
        result = QString(QObject::tr("Cannot handle HTTP status code %1")).
                arg(dwStatus);
    }

    return result;
}

bool Downloader::internetReadFileFully(HINTERNET resourceHandle,
        PVOID buffer, DWORD bufferSize, PDWORD bufferLength)
{
    DWORD alreadyRead = 0;
    bool result;
    while (true) {
        DWORD len;
        result = InternetReadFile(resourceHandle,
                ((char*) buffer) + alreadyRead,
                bufferSize - alreadyRead, &len);

        if (!result) {
            *bufferLength = alreadyRead;
            break;
        } else {
            alreadyRead += len;

            if (alreadyRead == bufferSize || len == 0) {
                *bufferLength = alreadyRead;
                break;
            }
        }
    }
    return result;
}

void Downloader::readDataGZip(Job* job, HINTERNET hResourceHandle, QFile* file,
        QString* sha1, int64_t contentLength, QCryptographicHash::Algorithm alg)
{
    QString initialTitle = job->getTitle();

    // download/compute SHA1 loop
    QCryptographicHash hash(alg);
    const int bufferSize = 512 * 1024;
    unsigned char* buffer = new unsigned char[bufferSize];
    const int buffer2Size = 512 * 1024;
    unsigned char* buffer2 = new unsigned char[buffer2Size];

    bool zlibStreamInitialized = false;
    z_stream d_stream;

    int err = 0;
    int64_t alreadyRead = 0;
    DWORD bufferLength;
    do {
        if (!internetReadFileFully(hResourceHandle, buffer,
                bufferSize, &bufferLength)) {
            QString errMsg;
            WPMUtils::formatMessage(GetLastError(), &errMsg);
            job->setErrorMessage(errMsg);
            break;
        }

        if (bufferLength == 0)
            break;

        // http://www.gzip.org/zlib/rfc-gzip.html
        if (!zlibStreamInitialized) {
            unsigned int cur = 0;

            /*
            if (cur + 10 > bufferLength) {
                job->setErrorMessage("Less than 10 bytes");
                goto out;
            }

            unsigned char flg = buffer[3];
            cur = 10;

            // FLG.FEXTRA
            if (flg & 4) {
                uint16_t xlen;
                if (cur + 2 > bufferLength) {
                    job->setErrorMessage("XLEN missing");
                    goto out;
                } else {
                    xlen = *((uint16_t*) (buffer + cur));
                    cur += 2;
                }

                if (cur + xlen > bufferLength) {
                    job->setErrorMessage("EXTRA missing");
                    goto out;
                } else {
                    cur += xlen;
                }
            }

            // FLG.FNAME
            if (flg & 8) {
                while (true) {
                    if (cur + 1 > bufferLength) {
                        job->setErrorMessage("FNAME missing");
                        goto out;
                    } else {
                        uint8_t c = *((uint8_t*) (buffer + cur));
                        cur++;
                        if (c == 0)
                            break;
                    }
                }
            }

            // FLG.FCOMMENT
            if (flg & 16) {
                while (true) {
                    if (cur + 1 > bufferLength) {
                        job->setErrorMessage("COMMENT missing");
                        goto out;
                    } else {
                        uint8_t c = *((uint8_t*) (buffer + cur));
                        cur++;
                        if (c == 0)
                            break;
                    }
                }
            }

            // FLG.FHCRC
            if (flg & 2) {
                if (cur + 2 > bufferLength) {
                    job->setErrorMessage("CRC16 missing");
                    goto out;
                } else {
                    cur += 2;
                }
            }*/

            d_stream.zalloc = (alloc_func) 0;
            d_stream.zfree = (free_func) 0;
            d_stream.opaque = (voidpf) 0;

            d_stream.next_in = buffer + cur;
            d_stream.avail_in = bufferLength - cur;
            d_stream.avail_out = buffer2Size;
            d_stream.next_out = buffer2;
            zlibStreamInitialized = true;

            // 15 = maximum buffer size, 32 = zlib and gzip formats are parsed
            int err = inflateInit2(&d_stream, 15 + 32);
            if (err != Z_OK) {
                job->setErrorMessage(QString(QObject::tr("zlib error %1")).
                        arg(err));
                break;
            }
        } else {
            d_stream.next_in = buffer;
            d_stream.avail_in = bufferLength;
        }

        // see http://zlib.net/zpipe.c
        do {
            d_stream.avail_out = buffer2Size;
            d_stream.next_out = buffer2;

            int err = inflate(&d_stream, Z_NO_FLUSH);
            if (err == Z_NEED_DICT) {
                job->setErrorMessage(QString(QObject::tr("zlib error %1")).
                        arg(err));
                err = Z_DATA_ERROR;
                inflateEnd(&d_stream);
                break;
            } else if (err == Z_MEM_ERROR || err == Z_DATA_ERROR) {
                job->setErrorMessage(QString(QObject::tr("zlib error %1")).
                        arg(err));
                inflateEnd(&d_stream);
                break;
            } else {
                if (sha1)
                    hash.addData((char*) buffer2,
                            buffer2Size - d_stream.avail_out);

                file->write((char*) buffer2,
                        buffer2Size - d_stream.avail_out);
            }
        } while (d_stream.avail_out == 0);

        if (!job->getErrorMessage().isEmpty())
            break;

        alreadyRead += bufferLength;
        if (contentLength > 0) {
            job->setProgress(((double) alreadyRead) / contentLength);
            job->setTitle(initialTitle + " / " +
                    QString(QObject::tr("%L0 of %L1 bytes")).
                    arg(alreadyRead).
                    arg(contentLength));
        } else {
            job->setProgress(0.5);
            job->setTitle(initialTitle + " / " +
                    QString(QObject::tr("%L0 bytes")).
                    arg(alreadyRead));
        }
    } while (bufferLength != 0 && !job->isCancelled());

    err = inflateEnd(&d_stream);
    if (err != Z_OK) {
        job->setErrorMessage(QString(QObject::tr("zlib error %1")).
                arg(err));
    }

    if (sha1 && !job->isCancelled() && job->getErrorMessage().isEmpty())
        *sha1 = hash.result().toHex().toLower();

// out:
    delete[] buffer;
    delete[] buffer2;

    if (!job->isCancelled() && job->getErrorMessage().isEmpty())
        job->setProgress(1);

    job->complete();
}

void Downloader::readDataFlat(Job* job, HINTERNET hResourceHandle, QFile* file,
        QString* sha1, int64_t contentLength, QCryptographicHash::Algorithm alg)
{
    QString initialTitle = job->getTitle();

    // download/compute SHA1 loop
    QCryptographicHash hash(alg);
    const int bufferSize = 512 * 1024;
    unsigned char* buffer = new unsigned char[bufferSize];

    int64_t alreadyRead = 0;
    DWORD bufferLength;
    do {
        // gzip-header is at least 10 bytes long
        if (!InternetReadFile(hResourceHandle, buffer,
                bufferSize, &bufferLength)) {
            QString errMsg;
            WPMUtils::formatMessage(GetLastError(), &errMsg);
            job->setErrorMessage(errMsg);
            break;
        }

        if (bufferLength == 0)
            break;

        // update SHA1 if necessary
        if (sha1)
            hash.addData((char*) buffer, bufferLength);

        if (file)
            file->write((char*) buffer, bufferLength);

        alreadyRead += bufferLength;
        if (contentLength > 0) {
            job->setProgress(((double) alreadyRead) / contentLength);
            job->setTitle(initialTitle + " / " +
                    QString(QObject::tr("%L0 of %L1 bytes")).
                    arg(alreadyRead).
                    arg(contentLength));
        } else {
            job->setProgress(0.5);
            job->setTitle(initialTitle + " / " +
                    QString(QObject::tr("%L0 bytes")).
                    arg(alreadyRead));
        }
    } while (bufferLength != 0 && !job->isCancelled());

    if (!job->isCancelled() && job->getErrorMessage().isEmpty())
        job->setProgress(1);

    if (sha1 && !job->isCancelled() && job->getErrorMessage().isEmpty())
        *sha1 = hash.result().toHex().toLower();

    delete[] buffer;

    job->complete();
}

void Downloader::readData(Job* job, HINTERNET hResourceHandle, QFile* file,
        QString* sha1, bool gzip, int64_t contentLength,
        QCryptographicHash::Algorithm alg)
{
    if (gzip && file)
        readDataGZip(job, hResourceHandle, file, sha1, contentLength, alg);
    else
        readDataFlat(job, hResourceHandle, file, sha1, contentLength, alg);
}

void Downloader::download(Job* job, const QUrl& url, QFile* file,
        QString* sha1, QCryptographicHash::Algorithm alg, bool useCache,
        QString *mime, bool keepConnection, int timeout, bool interactive)
{
    QString contentDisposition;

    if (url.scheme() == "https" || url.scheme() == "http")
        downloadWin(job, url, L"GET", file, mime, &contentDisposition,
                defaultPasswordWindow, sha1, useCache, alg, keepConnection,
                timeout, interactive);
    else if (url.toString().startsWith("data:image/png;base64,")) {
        if (file) {
            QString dataURL_ = url.toString().mid(22);
            QByteArray ba = QByteArray::fromBase64(dataURL_.toLatin1());
            if (file->write(ba) < 0)
                job->setErrorMessage(file->errorString());
        }
        job->complete();
    } else if (url.scheme() == "file") {
        QString localFile = url.toLocalFile();
        QFileInfo fi(localFile);
        if (fi.isAbsolute())
            copyFile(job, localFile, file, sha1,  alg);
        else {
            job->setErrorMessage(
                    QObject::tr("Cannot download a file from a relative path %1").
                    arg(localFile));
            job->complete();
        }
    } else {
        job->setErrorMessage(QObject::tr("Unsupported URL scheme: %1").
                arg(url.toDisplayString()));
        job->complete();
    }
}

void Downloader::copyFile(Job* job, const QString& source, QFile* file,
         QString* sha1, QCryptographicHash::Algorithm alg) {
    QFile srcFile(source);
    if (!srcFile.open(QFile::ReadOnly)) {
        job->setErrorMessage(QObject::tr("Error opening file: %1").
                arg(source));
    } else {
        qint64 srcSize = srcFile.size();
        const int SZ = 8192;
        char* data = new char[SZ];

        qint64 progress = 0;
        QCryptographicHash crypto(alg);
        while(true) {
            qint64 c = srcFile.read(data, SZ);
            if (c <= 0)
                break;

            if (sha1)
                crypto.addData(data, c);
            if (file)
                file->write(data, c);

            progress += c;
            if (srcSize != 0)
                job->setProgress(((double) progress) / srcSize);
        }

        if (sha1)
            *sha1 = crypto.result().toHex().toLower();

        srcFile.close();
        delete[] data;
    }
    job->complete();
}

Downloader::Response Downloader::download(Job *job,
        const Downloader::Request &request)
{
    Downloader::Response r;

    // TODO:
    // request.parentWindow
    // httpMethod
    download(job, request.url, request.file,
            request.hashSum ? &r.hashSum : 0, request.alg,
            request.useCache, &r.mimeType, request.keepConnection,
            request.timeout, request.interactive);

    return r;
}

int64_t Downloader::getContentLength(Job* job, const QUrl &url,
        HWND parentWindow)
{
    int64_t result = -1;
    if (url.scheme() == "file") {
        QString filename = url.toLocalFile();
        QFileInfo fi(filename);
        if (fi.isAbsolute()) {
            result = fi.size();
        } else {
            job->setErrorMessage(QObject::tr("Cannot process relative file name %1").
                    arg(filename));
        }
        job->complete();
    } else {
        result = downloadWin(job, url, L"HEAD", 0, 0, 0, parentWindow, 0, false,
                QCryptographicHash::Sha1, false, 15);
    }
    return result;
}

QTemporaryFile* Downloader::download(Job* job, const QUrl &url, QString* sha1,
        QCryptographicHash::Algorithm alg,
        bool useCache, QString *mime)
{
    QTemporaryFile* file = new QTemporaryFile();

    if (file->open()) {
        download(job, url, file, sha1, alg, useCache, mime);
        file->close();

        if (job->isCancelled() || !job->getErrorMessage().isEmpty()) {
            delete file;
            file = 0;
        }
    } else {
        job->setErrorMessage(QString(QObject::tr("Error opening file: %1")).
                arg(file->fileName()));
        delete file;
        file = 0;
        job->complete();
    }

    return file;
}

QTemporaryFile* Downloader::downloadToTemporary(Job* job,
        const Downloader::Request &request)
{
    QTemporaryFile* file = new QTemporaryFile();
    Downloader::Request r2(request);
    r2.file = file;

    if (file->open()) {
        download(job, r2);
        file->close();

        if (job->isCancelled() || !job->getErrorMessage().isEmpty()) {
            delete file;
            file = 0;
        }
    } else {
        job->setErrorMessage(QString(QObject::tr("Error opening file: %1")).
                arg(file->fileName()));
        delete file;
        file = 0;
        job->complete();
    }

    return file;
}

