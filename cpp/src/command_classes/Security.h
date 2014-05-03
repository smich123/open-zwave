//-----------------------------------------------------------------------------
//
//	Security.h
//
//	Implementation of the Z-Wave COMMAND_CLASS_Security
//
//	Copyright (c) 2010 Mal Lansell <openzwave@lansell.org>
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

#ifndef _Security_H
#define _Security_H


#include "CommandClass.h"


namespace OpenZWave
{
	/** \brief Implements COMMAND_CLASS_SECURITY (0x98), a Z-Wave device command class.
	 */

	typedef struct SecurityPayload {
		uint8 m_length;
		uint8 m_part;
		uint8 m_data[28];
	} SecurityPayload;

	/* This should probably go into its own file, but its so simple... and only the Security Command Class uses it currently
	 */

	class Timer {
	public:
		Timer() {
			this->Reset();
		};
		virtual ~Timer() {};
		void Reset() {
			start = clock();
		}
		uint64 GetMilliseconds() {
			return (( clock() - start ) / (double) CLOCKS_PER_SEC)/1000;
		}
	private:
		clock_t start;
	};

	class Security: public CommandClass
	{
	public:
		static CommandClass* Create( uint32 const _homeId, uint8 const _nodeId ){ return new Security( _homeId, _nodeId ); }
		virtual ~Security(){}

		static uint8 const StaticGetCommandClassId(){ return 0x98; }
		static string const StaticGetCommandClassName(){ return "COMMAND_CLASS_SECURITY"; }

		// From CommandClass
		virtual uint8 const GetCommandClassId()const{ return StaticGetCommandClassId(); }
		virtual string const GetCommandClassName()const{ return StaticGetCommandClassName(); }
		virtual bool HandleMsg( uint8 const* _data, uint32 const _length, uint32 const _instance = 1 );
		void SendMsg( Msg* _msg );
	private:
		Security( uint32 const _homeId, uint8 const _nodeId );
		bool RequestState( uint32 const _requestFlags, uint8 const _instance, Driver::MsgQueue const _queue);
		bool RequestValue( uint32 const _requestFlags, uint8 const _index, uint8 const _instance, Driver::MsgQueue const _queue);
		void SendNonceReport();
		void RequestNonce();
		void GenerateAuthentication( uint8 const* _data, uint32 const _length, uint8 const _sendingNode, uint8 const _receivingNode, uint8* _authentication);
		bool DecryptMessage( uint8 const* _data, uint32 const _length );
		bool EncryptMessage( uint8 const* _nonce );
		void QueuePayload( SecurityPayload const& _payload );

		Mutex *m_queueMutex;
		list<SecurityPayload>      m_queue;         // Messages waiting to be sent when the device wakes up
		bool m_waitingForNonce;
		uint8 m_initializationVector[16]; // First 8 Bytes are Random, Second 8 Bytes are the NONCE
		uint8 m_sequenceCounter;
		Timer m_nonceTimer;




	};

} // namespace OpenZWave

#endif

