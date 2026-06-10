#define LOG_TIME_FORMAT LOG_COLOR_GRAY "%H:%M:%S "

/*#define LOG_ENTRY_FILE "log_table.h" */

/*#define LOG_USE_DEF_FILE*/
/*#define LOG_USE_SHORT_NAMES_GLOBALLY*/
/*#define LOG_ENTRY_FILE "log.def"*/

#define LOG_ENTRIES \
LOG_ENTRY(SEVERITY,   INFO,         (1<< 1),    "[INFO ]",     LOG_COLOR_GREEN   )\
LOG_ENTRY(SEVERITY,   WARN,         (1<< 2),    "[WARN ]",     LOG_COLOR_YELLOW  )\
LOG_ENTRY(SEVERITY,   ERROR,        (1<< 3),    "[ERROR]",     LOG_COLOR_RED     )\
LOG_ENTRY(SEVERITY,   FATAL,        (1<< 4),    "[FATAL]",     LOG_COLOR_PURPLE  )\
LOG_ENTRY(SUBSYSTEM,  PLATFORM,     (1<<10),    "[PLATF]",     LOG_COLOR_CYAN    )\
LOG_ENTRY(SUBSYSTEM,  AUDIO,        (1<<11),    "[AUDIO]",     LOG_COLOR_GREEN   )\
LOG_ENTRY(SUBSYSTEM,  VIDEO,        (1<<12),    "[VIDEO]",     LOG_COLOR_RED     )\
LOG_ENTRY(CATEGORY,   INIT,         (1<<20),    "[INIT ]",     LOG_COLOR_GRAY    )\
LOG_ENTRY(CATEGORY,   SHUTDOWN,     (1<<21),    "[SHUTD]",     LOG_COLOR_GRAY    )\
LOG_ENTRY(CATEGORY,   EMPTY0,             0,    "[     ]",     LOG_COLOR_GRAY    )

#include "log.h"

int log_verbosity_level = LOG_EVERYTHING; /* define globally once */

int main()
{
    /* usage */
    LOG(INFO|PLATFORM|INIT,      "Started engine");
    LOG(VIDEO|INIT,              "Successfully initialized video subsystem");

    unsigned int buffer_size = 1024;
    LOG(AUDIO|WARN|INIT,         "Insufficient memory for buffer of size %u", buffer_size);

    /* only log warnings or worse from here */
    LOG_SET_MASK(WARN | ERROR | FATAL); /* short form of log_verbosity_level = LOG_SEVERITY_WARN | LOG_SEVERITY_ERROR | LOG_SEVERITY_FATAL; */

    LOG(INFO|VIDEO,              "Won't be displayed");
    LOG(FATAL|PLATFORM|SHUTDOWN, "Fatal error occured. Aborting...");
}
