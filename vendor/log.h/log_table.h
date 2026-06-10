#define LOG_ENTRIES \
LOG_ENTRY(SEVERITY,   TRACE,        (1<< 0),    "[TRACE]",     LOG_COLOR_GRAY    )\
LOG_ENTRY(SEVERITY,   INFO,         (1<< 1),    "[INFO ]",     LOG_COLOR_GREEN   )\
LOG_ENTRY(SEVERITY,   WARN,         (1<< 2),    "[WARN ]",     LOG_COLOR_YELLOW  )\
LOG_ENTRY(SEVERITY,   ERROR,        (1<< 3),    "[ERROR]",     LOG_COLOR_RED     )\
LOG_ENTRY(SEVERITY,   FATAL,        (1<< 4),    "[FATAL]",     LOG_COLOR_PURPLE  )\
LOG_ENTRY(SUBSYSTEM,  PLATFORM,     (1<< 8),    "[PLATF]",     LOG_COLOR_CYAN    )\
LOG_ENTRY(SUBSYSTEM,  AUDIO,        (1<< 9),    "[AUDIO]",     LOG_COLOR_GREEN   )\
LOG_ENTRY(SUBSYSTEM,  VIDEO,        (1<<10),    "[VIDEO]",     LOG_COLOR_RED     )\
LOG_ENTRY(SUBSYSTEM,  NETWORK,      (1<<11),    "[NETWK]",     LOG_COLOR_PURPLE  )\
LOG_ENTRY(SUBSYSTEM,  TEST,         (1<<12),    "[TEST ]",     LOG_COLOR_GREEN   )\
LOG_ENTRY(CATEGORY,   INIT,         (1<<18),    "[INIT ]",     LOG_COLOR_GRAY    )\
LOG_ENTRY(CATEGORY,   SHUTDOWN,     (1<<19),    "[SHUTD]",     LOG_COLOR_GRAY    )\
LOG_ENTRY(CATEGORY,   ACTIVATE,     (1<<20),    "[ACTIV]",     LOG_COLOR_GRAY    )\
LOG_ENTRY(CATEGORY,   DEACTIVATE,   (1<<21),    "[DEACT]",     LOG_COLOR_GRAY    )\
LOG_ENTRY(CATEGORY,   EMPTY0,             0,    "[     ]",     LOG_COLOR_GRAY    )
