#pragma once

#if defined(LOG_ENTRY_FILE) && !defined(LOG_USE_DEF_FILE)
  #include LOG_ENTRY_FILE
#endif

#if defined(_MSC_VER)
  __pragma(warning(disable : 4996)) /* 'localtime' is deprecated */
#endif
#if defined (_WIN32)
  #undef ERROR /* defined in wingdi.h as 0 */
#endif

#include <stdio.h>
#include <time.h>

#if !defined(LOG_VARIABLE_NAME)
  #define LOG_VARIABLE_NAME log_verbosity_level
#endif

extern int LOG_VARIABLE_NAME;

#ifdef LOG_USE_NO_COLOR
  #define LOG_COLOR_OFF    ""
  #define LOG_COLOR_GRAY   ""
  #define LOG_COLOR_BLUE   ""
  #define LOG_COLOR_CYAN   ""
  #define LOG_COLOR_GREEN  ""
  #define LOG_COLOR_YELLOW ""
  #define LOG_COLOR_RED    ""
  #define LOG_COLOR_PURPLE ""
#else
  #define LOG_COLOR_OFF    "\x1b[0m"
  #define LOG_COLOR_GRAY   "\x1b[38;5;245m"
  #define LOG_COLOR_BLUE   "\x1b[94m"
  #define LOG_COLOR_CYAN   "\x1b[36m"
  #define LOG_COLOR_GREEN  "\x1b[32m"
  #define LOG_COLOR_YELLOW "\x1b[33m"
  #define LOG_COLOR_RED    "\x1b[31m"
  #define LOG_COLOR_PURPLE "\x1b[35m"
#endif

enum
{
  /* severity */
  #define SEVERITY  1
  #define SUBSYSTEM 0
  #define CATEGORY  0
  #define LOG_ENTRY(entry, name, value, string, color) LOG_SEVERITY_##name = value * entry ,
  #ifdef LOG_USE_DEF_FILE
    #include LOG_ENTRY_FILE
  #else
    LOG_ENTRIES
  #endif
  #undef LOG_ENTRY

  #define LOG_ENTRY(entry, name, value, string, color) (value * entry) |
  #define LOG_SEVERITY_MASK LOG_ENTRIES 0
  #ifdef LOG_USE_DEF_FILE
    LOG_SEVERITY =
      #include LOG_ENTRY_FILE
    0,
  #else
    LOG_SEVERITY = LOG_SEVERITY_MASK,
  #endif
  #undef LOG_ENTRY
  #undef SEVERITY
  #undef SUBSYSTEM
  #undef CATEGORY

  /* subsystems */
  #define SEVERITY  0
  #define SUBSYSTEM 1
  #define CATEGORY  0
  #define LOG_ENTRY(entry, name, value, string, color) LOG_SUBSYSTEM_##name = value * entry,
  #ifdef LOG_USE_DEF_FILE
    #include LOG_ENTRY_FILE
  #else
    LOG_ENTRIES
  #endif
  #undef LOG_ENTRY

  #define LOG_ENTRY(entry, name, value, string, color) (value * entry) |
  #define LOG_SUBSYSTEM_MASK LOG_ENTRIES 0
  #ifdef LOG_USE_DEF_FILE
    LOG_SUBSYSTEMS =
      #include LOG_ENTRY_FILE
    0,
  #else
    LOG_SUBSYSTEMS = LOG_SUBSYSTEM_MASK,
  #endif
  #undef LOG_ENTRY
  #undef SEVERITY
  #undef SUBSYSTEM
  #undef CATEGORY

  /* categories */
  #define SEVERITY  0
  #define SUBSYSTEM 0
  #define CATEGORY  1
  #define LOG_ENTRY(entry, name, value, string, color) LOG_CATEGORY_##name = value * entry,
  #ifdef LOG_USE_DEF_FILE
    #include LOG_ENTRY_FILE
  #else
    LOG_ENTRIES
  #endif
  #undef LOG_ENTRY

  #define LOG_ENTRY(entry, name, value, string, color) (value * entry) |
  #define LOG_CATEGORY_MASK LOG_ENTRIES 0
  #ifdef LOG_USE_DEF_FILE
    LOG_CATEGORIES =
      #include LOG_ENTRY_FILE
    0,
  #else
    LOG_CATEGORIES = LOG_CATEGORY_MASK,
  #endif
  #undef LOG_ENTRY
  #undef SEVERITY
  #undef SUBSYSTEM
  #undef CATEGORY

  /* useful masks */
  LOG_EVERYTHING       = 0x7fffffff,
};

#if !defined(LOG_TIME_FORMAT)
  #define LOG_TIME_FORMAT "%H:%M:%S "
#endif

/* the core of the log macro */
#define _LOG(flags, format, ...)                                                                  \
  if ((flags) & LOG_VARIABLE_NAME)                                                                \
  {                                                                                               \
    /* get a timestamp string */                                                                  \
    time_t t = time(NULL); struct tm* time = localtime(&t);                                       \
    char buf[32]; buf[strftime(buf, sizeof(buf), LOG_TIME_FORMAT, time)] = '\0';                  \
    printf("%s" LOG_COLOR_OFF "%s%s%s %12.12s:%4i " format LOG_COLOR_OFF"\n", buf,                \
           _log_label((flags) & LOG_SEVERITY),                                                    \
           _log_label((flags) & LOG_SUBSYSTEMS),                                                  \
           _log_label((flags) & LOG_CATEGORIES),                                                  \
           __FILE__, __LINE__, ##__VA_ARGS__);                                                    \
  }

#if defined(LOG_USE_SHORT_NAMES_GLOBALLY) && defined(LOG_USE_DEF_FILE)
  /* NOTE fill the global namespace with unprefixed names of log entries (e.g. TRACE instead of LOG_SEVERITY_TRACE) */
  #define LOG_ENTRY(entry, name, value, string, color)  name = LOG_##entry##_##name ,
  enum {
      #include LOG_ENTRY_FILE
  };
  #undef LOG_ENTRY
#endif

#define LOG_ENTRY(entry, name, value, string, color)  name = LOG_##entry##_##name ,
#if defined(LOG_USE_DEF_FILE)
  #define LOG(flags, format, ...)          \
    {                                      \
        _LOG(flags, format, ##__VA_ARGS__) \
    }

  #define LOG_SET_MASK(flags) LOG_VARIABLE_NAME = flags;
#else
  #define LOG_SET_MASK(flags)               \
    {                                       \
        /* remove LOG_ prefix */            \
        enum { LOG_ENTRIES };               \
        LOG_VARIABLE_NAME = flags;          \
    }

  #define LOG(flags, format, ...)           \
    {                                       \
        /* remove LOG_ prefix */            \
        enum { LOG_ENTRIES };               \
        _LOG(flags, format, ##__VA_ARGS__)  \
    }
#endif
/* NOTE redefined to above #define at bottom of file */
#undef LOG_ENTRY

#define LOG_ENTRY(entry, name, value, string, color) case LOG_##entry##_##name: return color string LOG_COLOR_OFF;
static const char* _log_label(int flags)
{
  switch (flags)
  {
    #ifdef LOG_USE_DEF_FILE
      #include LOG_ENTRY_FILE
    #else
      LOG_ENTRIES
    #endif
    default: return "[     ]"; /* TODO let user pass in */
  }
}
#undef LOG_ENTRY

/* NOTE this #define is used in the LOG macro, so we cannot keep it #undef'ed */
#define LOG_ENTRY(entry, name, value, string, color)  name = LOG_##entry##_##name,
