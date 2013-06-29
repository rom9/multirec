static FILE *logFile;
static enum LogLevel { OFF=0, ERROR, DEBUG } logLevel = DEBUG;

static inline void initLogging(FILE *f, enum LogLevel l) {
	logFile = f;
	logLevel = l;
}

// ***** Logging implementation *****
#define LOG_IMPL    va_list args; \
					va_start( args, fmt ); \
					vfprintf(logFile, fmt, args); \
					va_end(args);


static inline void _do_log(const char *fmt, ...)
{
	LOG_IMPL
}


static inline void log_debug(const char *fmt, ...)
{
	if(logLevel<DEBUG)
		return;
	LOG_IMPL
	fflush(logFile);
}

static inline void log_dev_debug(MRDevice *c, const char *fmt, ...)
{
	if(logLevel<DEBUG)
		return;
	_do_log("DBG- dev %d : ", c->idx);
	LOG_IMPL
	fflush(logFile);
}


static inline void log_error(const char *fmt, ...)
{
	LOG_IMPL
	fflush(logFile);
}

static inline void log_dev_error(MRDevice *c, const char *fmt, ...)
{
	_do_log("ERR- dev %d : ", c->idx);
	LOG_IMPL
	fflush(logFile);
}
