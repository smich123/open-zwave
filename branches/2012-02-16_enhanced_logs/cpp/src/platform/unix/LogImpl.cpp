//-----------------------------------------------------------------------------
//
//	LogImpl.cpp
//
//  Unix implementation of message and error logging
//
//	Copyright (c) 2010, Greg Satz <satz@iranger.com>
//	All rights reserved.
//
//	SOFTWARE NOTICE AND LICENSE
//
//	This file is part of OpenZWave.
//
//	OpenZWave is free software: you can redistribute it and/or modify
//	it under the terms of the GNU Lesser General Public License as published
//	by the Free Software Foundation, either version 3 of the License,
//	or (at your option) any later version.
//
//	OpenZWave is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//	GNU Lesser General Public License for more details.
//
//	You should have received a copy of the GNU Lesser General Public License
//	along with OpenZWave.  If not, see <http://www.gnu.org/licenses/>.
//
//-----------------------------------------------------------------------------
#include <string>
#include "Defs.h"
#include "LogImpl.h"

using namespace OpenZWave;

//-----------------------------------------------------------------------------
//	<LogImpl::LogImpl>
//	Constructor
//-----------------------------------------------------------------------------
LogImpl::LogImpl
(
	string const& _filename,
	bool const _bAppendLog,
	bool const _bConsoleOutput,
	LogLevel const _saveLevel,
	LogLevel const _queueLevel,
	LogLevel const _dumpTrigger
):
	m_filename( _filename ),					// name of log file
	m_bConsoleOutput( _bConsoleOutput ),		// true to provide a copy of output to console
	m_bAppendLog( _bAppendLog ),				// true to append (and not overwrite) any existing log
	m_saveLevel( _saveLevel ),					// level of messages to log to file
	m_queueLevel( _queueLevel ),				// level of messages to log to queue
	m_dumpTrigger( _dumpTrigger )				// dump queued messages when this level is seen
{
	string accessType;

	// create an adjusted file name and timestamp string
	string timeStr = GetTimeStampString();

	if ( m_bAppendLog )
	{
		accessType = "a";
	}
	else
	{
		accessType = "w";
	}

	FILE* pFile = fopen( m_filename.c_str(), accessType.c_str() );
	if ( pFile != NULL )
	{
		fprintf( pFile, "\nLogging started %s\n\n", timeStr.c_str() );
		fclose( pFile );
	}
	setlinebuf(stdout);	// To prevent buffering and lock contention issues
}

//-----------------------------------------------------------------------------
//	<LogImpl::~LogImpl>
//	Destructor
//-----------------------------------------------------------------------------
LogImpl::~LogImpl
(
)
{
}

//-----------------------------------------------------------------------------
//	<LogImpl::Write>
//	Write to the log
//-----------------------------------------------------------------------------
void LogImpl::Write
( 
	LogLevel _logLevel,
	char const* _format, 
	va_list _args
)
{
	// create a timestamp string
	string timeStr = GetTimeStampString();

	// handle this message
	if( (_logLevel <= m_queueLevel) || (_logLevel == LogLevel_Internal) )	// we're going to do something with this message...
	{
		char lineBuf[1024];
		if( !_format || ( _format[0] == 0 ) )
			strcpy( lineBuf, "" );
		else
		{
			va_list saveargs;
			va_copy( saveargs, _args );
			vsprintf( lineBuf, _format, _args );			// GREG: do you have vsprintf_s?
			va_end( saveargs );
		}

		// should this message be saved to file (and possibly written to console?)
		if( (_logLevel <= m_saveLevel) || (_logLevel == LogLevel_Internal) )
		{
			// save to file
			FILE* pFile = fopen( m_filename.c_str(), "a" );
			if ( pFile != NULL )
			{
				if( _logLevel != LogLevel_Internal )						// don't add a second timestamp to display of queued messages
				{
					// GREG:  Do you need the c_str()?  
					fprintf( pFile, "%s", timeStr.c_str() );
					if( m_bConsoleOutput )
					{
						printf( "%s", timeStr.c_str() );
					}
				}

				// print message to file (and possibly screen)
				fprintf( pFile, "%s", lineBuf );
				if( m_bConsoleOutput )
				{
					printf( "%s", lineBuf );
				}

				// add a newline
				putc( '\n', pFile ); 
				if( m_bConsoleOutput )
				{
					putchar( '\n' );
				}

				fclose( pFile );
			}
		}

		if( _logLevel != LogLevel_Internal )
		{
			char queueBuf[1024];
			string threadStr = GetThreadId();
			snprintf( queueBuf, sizeof(queueBuf), "%s%s%s", timeStr.c_str(), threadStr.c_str(), lineBuf );
			Queue( queueBuf );
		}
	}

	// now check to see if the _dumpTrigger has been hit
	if( (_logLevel <= m_dumpTrigger) && (_logLevel != LogLevel_Internal) && (_logLevel != LogLevel_Always) )
		QueueDump();
}

//-----------------------------------------------------------------------------
//	<LogImpl::Queue>
//	Write to the log queue
//-----------------------------------------------------------------------------
void LogImpl::Queue
( 
	char const* _buffer
)
{
	string bufStr = _buffer;
	m_logQueue.push_back( bufStr );

	// rudimentary queue size management
	if( m_logQueue.size() > 500 )
	{
		m_logQueue.pop_front();
	}
}

//-----------------------------------------------------------------------------
//	<LogImpl::QueueDump>
//	Dump the LogQueue to output device
//-----------------------------------------------------------------------------
void LogImpl::QueueDump
( 
)
{
	Log::Write( LogLevel_Internal, "\n\nDumping queued log messages\n");
	list<string>::iterator it = m_logQueue.begin();
	while( it != m_logQueue.end() )
	{
		string strTemp = *it;
		Log::Write( LogLevel_Internal, strTemp.c_str() );
		it++;
	}
	m_logQueue.clear();
	Log::Write( LogLevel_Internal, "\nEnd of queued log message dump\n\n");
}

//-----------------------------------------------------------------------------
//	<LogImpl::Clear>
//	Clear the LogQueue
//-----------------------------------------------------------------------------
void LogImpl::QueueClear
( 
)
{
	m_logQueue.clear();
}

//-----------------------------------------------------------------------------
//	<LogImpl::SetLoggingState>
//	Sets the various log state variables
//-----------------------------------------------------------------------------
void LogImpl::SetLoggingState
(
	LogLevel _saveLevel,
	LogLevel _queueLevel,
	LogLevel _dumpTrigger
)
{
	m_saveLevel = _saveLevel;
	m_queueLevel = _queueLevel;
	m_dumpTrigger = _dumpTrigger;
}

//-----------------------------------------------------------------------------
//	<LogImpl::GetTimeStampAndThreadId>
//	Generate a string with formatted current time
//-----------------------------------------------------------------------------
string LogImpl::GetTimeStampString
( 
)
{
	// Get a timestamp
	struct timeval tv;
	gettimeofday(&tv, NULL);
	struct tm *tm;
	tm = localtime( &tv.tv_sec );

	// create a time stamp string for the log message
	char buf[100];
	snprintf( buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d:%03d ", 
		tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
		  tm->tm_hour, tm->tm_min, tm->tm_sec, tv.tv_usec / 1000 );
	string str = buf;
	return str;
}

//-----------------------------------------------------------------------------
//	<LogImpl::GetThreadId>
//	Generate a string with formatted thread id
//-----------------------------------------------------------------------------
string LogImpl::GetThreadId
( 
)
{
	char buf[20];
	// GREG:  Not sure how to get a threadId
	snprintf( buf, sizeof(buf), "%08x ", pthread_self() );
	string str = buf;
	return str;
}

//-----------------------------------------------------------------------------
//	<LogImpl::SetLogFileName>
//	Provide a new log file name (applicable to future writes)
//-----------------------------------------------------------------------------
void LogImpl::SetLogFileName
( 
	string _filename
)
{
	m_filename = _filename;
}