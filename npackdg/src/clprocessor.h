#ifndef CLPROCESSOR_H
#define CLPROCESSOR_H

#include <QString>
#include <QThread>
#include <QList>

#include "commandline.h"
#include "job.h"
#include "installoperation.h"

/**
 * @brief process the command line
 */
class CLProcessor
{
    CommandLine cl;
public:
    /**
     * @brief -
     */
    CLProcessor();

    /**
     * @brief installs packages
     * @return error message or ""
     */
    QString add();

    /**
     * @brief removes packages
     * @return error message or ""
     */
    QString remove();

    /**
     * @brief updates packages
     * @return error message or ""
     */
    QString update();

    /**
     * @brief show the command line options help
     */
    void usage();

    /**
     * @brief process the command line
     * @param errorCode error code will be stored here
     * @return false = GUI should be started
     */
    bool process(int *errorCode);
private:
    /**
     * @param install the objects will be destroyed
     * @param programCloseType how the programs should be closed
     * @return error message or ""
     */
    QString process(QList<InstallOperation*>& install, int programCloseType);

    /**
     * @brief waits for a job
     * @param job the object will be destroyed
     */
    void monitorAndWaitFor(Job *job);

    QString startNewestNpackdg();
};

#endif // CLPROCESSOR_H
