//-----------------------------------------------------------------------------
//
//	Driver.cpp
//
//	Communicates with a Z-Wave network
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

#include "Defs.h"
#include "Driver.h"
#include "Options.h"
#include "Manager.h"
#include "Node.h"
#include "Msg.h"
#include "Notification.h"

#include "Event.h"
#include "Mutex.h"
#include "SerialController.h"
#include "HidController.h"
#include "Thread.h"
#include "Log.h"
#include "TimeStamp.h"

#include "CommandClasses.h"
#include "ApplicationStatus.h"
#include "ControllerReplication.h"
#include "WakeUp.h"
#include "SwitchAll.h"
#include "ManufacturerSpecific.h"

#include "ValueID.h"
#include "Value.h"

#include <algorithm>

using namespace OpenZWave;

// Version numbering for saved configurations. Any change that will invalidate
// previously saved configurations must be accompanied by an increment to the
// version number, and a comment explaining the date of, and reason for, the change.
//
// 01: 12-31-2010 - Introduced config version numbering due to ValueID format change.
// 02: 01-12-2011 - Command class m_afterMark sense corrected, and attribute named to match.
// 03: 08-04-2011 - Changed command class instance handling for non-sequential MultiChannel endpoints.
//
uint32 const c_configVersion = 3;

#define MAX_TRIES		3		// Retry sends up to 3 times
#define RETRY_TIMEOUT	2000	// Retry send after two seconds

static char const* c_libraryTypeNames[] = 
{
	"Unknown",					// library type 0
	"Static Controller",		// library type 1
	"Controller",       		// library type 2
	"Enhanced Slave",   		// library type 3
	"Slave",            		// library type 4
	"Installer",				// library type 5
	"Routing Slave",			// library type 6
	"Bridge Controller",    	// library type 7
	"Device Under Test"			// library type 8
};

//-----------------------------------------------------------------------------
// <Driver::Driver>
// Constructor
//-----------------------------------------------------------------------------
Driver::Driver
( 
	string const& _controllerPath,
	ControllerInterface const& _interface
):
	m_driverThread( new Thread( "driver" ) ),
	m_exit( false ),
	m_init( false ),
	m_awakeNodesQueried( false ),
	m_allNodesQueried( false ),
	m_notifytransactions( false ),
	m_controllerPath( _controllerPath ),
	m_homeId( 0 ),
	m_initCaps( 0 ),
	m_controllerCaps( 0 ),
	m_nodeMutex( new Mutex() ),
	m_controllerReplication( NULL ),
	m_sendMutex( new Mutex() ),
	m_waitingForAck( false ),
	m_expectedCallbackId( 0 ),
	m_expectedReply( 0 ),
	m_expectedCommandClassId( 0 ),
	m_expectedNodeId( 0 ),
	m_pollThread( new Thread( "poll" ) ),
	m_pollMutex( new Mutex() ),
	m_pollInterval( 30 ),                   // By default, every polled device is queried once every 30 seconds
	m_controllerState( ControllerState_Normal ),
	m_controllerCommand( ControllerCommand_None ),
	m_controllerCallback( NULL ),
	m_controllerCallbackContext( NULL ),
	m_controllerAdded( false ),
	m_controllerCommandNode( 0 ),
	m_SOFCnt( 0 ),
	m_ACKWaiting( 0 ),
	m_readAborts( 0 ),
	m_badChecksum( 0 ),
	m_readCnt( 0 ),
	m_writeCnt( 0 ),
	m_CANCnt( 0 ),
	m_NAKCnt( 0 ),
	m_ACKCnt( 0 ),
	m_OOFCnt( 0 ),
	m_controllerReadCnt( 0 ),
	m_controllerWriteCnt( 0 )
{
	// Create the message queue events
	for( int32 i=0; i<MsgQueue_Count; ++i )
	{
		m_queueEvent[i] = new Event();
	}

	// Clear the nodes array
	memset( m_nodes, 0, sizeof(Node*) * 256 );
    
	if( ControllerInterface_Hid == _interface )
	{
		m_controller = new HidController();
	}
	else
	{
		m_controller = new SerialController();
	}
	m_controller->SetSignalThreshold( 1 );

	Options::Get()->GetOptionAsBool( "NotifyTransactions", &m_notifytransactions );
}

//-----------------------------------------------------------------------------
// <Driver::Driver>
// Destructor
//-----------------------------------------------------------------------------
Driver::~Driver
(
)
{
	// Save the driver config before deleting anything else
	WriteConfig();

	// The order of the statements below has been achieved by mitigating freed memory
	//references using a memory allocator checker. Do not rearrange unless you are
	//certain memory won't be referenced out of order. --Greg Satz, April 2010
	m_exit = true;

	m_pollThread->Stop();
	m_pollThread->Release();

	m_driverThread->Stop();
	m_driverThread->Release();

	m_sendMutex->Release();
	m_pollMutex->Release();

	m_controller->Close();
	m_controller->Release();

	// Clear the send Queue
	for( int32 i=0; i<MsgQueue_Count; ++i )
	{
		while( !m_msgQueue[i].empty() )
		{
			MsgQueueItem const& item = m_msgQueue[i].front();
			if( MsgQueueCmd_SendMsg == item.m_command )
			{
				delete item.m_msg;
			}
			m_msgQueue[i].pop_front();
		}

		m_queueEvent[i]->Release();
	}

	// Clear the node data
	LockNodes();
	for( int i=0; i<256; ++i )
	{
		if( GetNodeUnsafe( i ) )
		{
			Notification* notification = new Notification( Notification::Type_NodeRemoved );
			notification->SetHomeAndNodeIds( m_homeId, i );
			QueueNotification( notification ); 

			delete m_nodes[i];
			m_nodes[i] = NULL;
		}
	}
	ReleaseNodes();

	NotifyWatchers();
	m_nodeMutex->Release();
    
    // Unsure at what point this is safe to do?
    delete m_controllerReplication;
}

//-----------------------------------------------------------------------------
// <Driver::Start>
// Start the driver thread
//-----------------------------------------------------------------------------
void Driver::Start
(
)
{
	// Start the thread that will handle communications with the Z-Wave network
	m_driverThread->Start( Driver::DriverThreadEntryPoint, this );
}

//-----------------------------------------------------------------------------
// <Driver::DriverThreadEntryPoint>
// Entry point of the thread for creating and managing the worker threads
//-----------------------------------------------------------------------------
void Driver::DriverThreadEntryPoint
( 
	Event* _exitEvent,
	void* _context 
)
{
	Driver* driver = (Driver*)_context;
	if( driver )
	{
		driver->DriverThreadProc( _exitEvent );
	}
}

//-----------------------------------------------------------------------------
// <Driver::DriverThreadProc>
// Create and manage the worker threads
//-----------------------------------------------------------------------------
void Driver::DriverThreadProc
( 
	Event* _exitEvent
)
{
	uint32 attempts = 0;
	while( true )
	{
		if( Init( attempts ) )
		{
			// Driver has been initialised
			Wait* waitObjects[7];
			waitObjects[0] = _exitEvent;						// Thread must exit.
			waitObjects[1] = m_controller;						// Controller has received data.
			waitObjects[2] = m_queueEvent[MsgQueue_Command];	// A controller command is in progress.
			waitObjects[3] = m_queueEvent[MsgQueue_WakeUp];		// A node has woken. Pending messages should be sent.
			waitObjects[4] = m_queueEvent[MsgQueue_Send];		// Ordinary requests to be sent.
			waitObjects[5] = m_queueEvent[MsgQueue_Query];		// Node queries are pending.
			waitObjects[6] = m_queueEvent[MsgQueue_Poll];		// Poll request is waiting.

			TimeStamp retryTimeStamp;

			while( true )
			{
				uint32 count = 7;
				int32 timeout = Wait::Timeout_Infinite;

				// If we're waiting for a message to complete, we can only
				// handle incoming data and exit events.
				if( m_waitingForAck || m_expectedCallbackId || m_expectedReply )
				{
					count = 2;
					timeout = retryTimeStamp.TimeRemaining();
					if( timeout < 0 )
					{
						timeout = 0;
					}
				}

				// Wait for something to do
				int32 res = Wait::Multiple( waitObjects, count, timeout );
				switch( res )
				{
					case -1:	
					{
						// Wait has timed out - time to resend
						if( WriteMsg() )
						{
							retryTimeStamp.SetTime( RETRY_TIMEOUT );
						}
						break;
					}
					case 0:
					{
						// Exit has been signalled
						return;
					}
					case 1:
					{
						// Data has been received
						ReadMsg();
						break;
					}
					default:
					{
						// All the other events are sending message queue items
						if( WriteNextMsg( (MsgQueue)(res-2) ) )
						{
							retryTimeStamp.SetTime( RETRY_TIMEOUT );
						}
						break;
					}
				}

				// Send any pending notifications
				NotifyWatchers();
			}
		}

		++attempts;
		
		uint32 maxAttempts = 0;
		Options::Get()->GetOptionAsInt("DriverMaxAttempts", (int32 *)&maxAttempts);
		if( maxAttempts && (attempts >= maxAttempts) )
		{
			Manager::Get()->Manager::SetDriverReady(this, false);
			NotifyWatchers();
			break;
		}
			
		if( attempts < 25 )
		{
			// Retry every 5 seconds for the first two minutes
			if( !Wait::Single( _exitEvent, 5000 ) )
			{
				// Exit signalled.
				return;
			}
		}
		else
		{
			// Retry every 30 seconds after that
			if( !Wait::Single( _exitEvent, 30000 ) )
			{
				// Exit signalled.
				return;
			}
		}
	}
}


//-----------------------------------------------------------------------------
// <Driver::Init>
// Initialize the controller
//-----------------------------------------------------------------------------
bool Driver::Init
(
	uint32 _attempts
)
{
	m_nodeId = -1;
	m_waitingForAck = false;

	// Open the controller
	Log::Write( "  Opening controller %s", m_controllerPath.c_str() );

	if( !m_controller->Open( m_controllerPath ) )
	{
	 	Log::Write( "Failed to init the controller (attempt %d)", _attempts );
		return false;
	}

	// Controller opened successfully, so we need to start all the worker threads
	m_pollThread->Start( Driver::PollThreadEntryPoint, this );

	// Send a NAK to the ZWave device
	uint8 nak = NAK;
	m_controller->Write( &nak, 1 );
 
	// Get/set ZWave controller information in its preferred initialization order
	m_controller->PlayInitSequence( this );

	//If we ever want promiscuous mode uncomment this code.
	//Msg* msg = new Msg( "FUNC_ID_ZW_SET_PROMISCUOUS_MODE", 0xff, REQUEST, FUNC_ID_ZW_SET_PROMISCUOUS_MODE, false, false );
	//msg->Append( 0xff );
	//SendMsg( msg );
	
	// Init successful
	return true;
}

//-----------------------------------------------------------------------------
//	Configuration
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// <Driver::ReadConfig>
// Read our configuration from an XML document
//-----------------------------------------------------------------------------
bool Driver::ReadConfig
(
)
{
	char str[32];
	int32 intVal;

	// Load the XML document that contains the driver configuration
	string userPath;
	Options::Get()->GetOptionAsString( "UserPath", &userPath );
	
	snprintf( str, sizeof(str), "zwcfg_0x%08x.xml", m_homeId );
	string filename =  userPath + string(str);

	TiXmlDocument doc;
	if( !doc.LoadFile( filename.c_str(), TIXML_ENCODING_UTF8 ) )
	{
		return false;
	}

	TiXmlElement const* driverElement = doc.RootElement();

	// Version
	if( TIXML_SUCCESS != driverElement->QueryIntAttribute( "version", &intVal ) || (uint32)intVal != c_configVersion )
	{
		Log::Write( "Driver::ReadConfig - %s is from an older version of OpenZWave and cannot be loaded.", filename.c_str() );
		return false;
	}
	
	// Home ID
	char const* homeIdStr = driverElement->Attribute( "home_id" );
	if( homeIdStr )
	{
		char* p;
		uint32 homeId = (uint32)strtol( homeIdStr, &p, 0 );

		if( homeId != m_homeId )
		{
			Log::Write( "Driver::ReadConfig - Home ID in file %s is incorrect", filename.c_str() );
			return false;
		}
	}
	else
	{
		Log::Write( "Driver::ReadConfig - Home ID is missing from file %s", filename.c_str() );
		return false;
	}

	// Node ID
	if( TIXML_SUCCESS == driverElement->QueryIntAttribute( "node_id", &intVal ) )
	{
		if( (uint8)intVal != m_nodeId )
		{
			Log::Write( "Driver::ReadConfig - Controller Node ID in file %s is incorrect", filename.c_str() );
			return false;
		}
	}
	else
	{
		Log::Write( "Driver::ReadConfig - Node ID is missing from file %s", filename.c_str() );
		return false;
	}

	// Capabilities
	if( TIXML_SUCCESS == driverElement->QueryIntAttribute( "api_capabilities", &intVal ) )
	{
		m_initCaps = (uint8)intVal;
	}

	if( TIXML_SUCCESS == driverElement->QueryIntAttribute( "controller_capabilities", &intVal ) )
	{
		m_controllerCaps = (uint8)intVal;
	}

	// Poll Interval
	if( TIXML_SUCCESS == driverElement->QueryIntAttribute( "poll_interval", &intVal ) )
	{
		m_pollInterval = intVal;
	}

	// Read the nodes
	LockNodes();
	TiXmlElement const* nodeElement = driverElement->FirstChildElement();
	while( nodeElement )
	{
		char const* str = nodeElement->Value();
		if( str && !strcmp( str, "Node" ) )
		{
			// Get the node Id from the XML
			if( TIXML_SUCCESS == nodeElement->QueryIntAttribute( "id", &intVal ) )
			{
				uint8 nodeId = (uint8)intVal;
				Node* node = new Node( m_homeId, nodeId );
				m_nodes[nodeId] = node;

				Notification* notification = new Notification( Notification::Type_NodeAdded );
				notification->SetHomeAndNodeIds( m_homeId, nodeId );
				QueueNotification( notification ); 

				// Read the rest of the node configuration from the XML
				node->ReadXML( nodeElement );
			}
		}

		nodeElement = nodeElement->NextSiblingElement();
	}
	
	ReleaseNodes();
	return true;
}

//-----------------------------------------------------------------------------
// <Driver::WriteConfig>
// Write ourselves to an XML document
//-----------------------------------------------------------------------------
void Driver::WriteConfig
(
)
{
	char str[32];

	if (!m_homeId) {
		Log::Write("Warning: Tried to write driver config with no home ID set");
		return;
	}
	
	// Create a new XML document to contain the driver configuration
	TiXmlDocument doc;
	TiXmlDeclaration* decl = new TiXmlDeclaration( "1.0", "utf-8", "" );
	TiXmlElement* driverElement = new TiXmlElement( "Driver" );
	doc.LinkEndChild( decl );
	doc.LinkEndChild( driverElement );  

	snprintf( str, sizeof(str), "%d", c_configVersion );
	driverElement->SetAttribute( "version", str );

	snprintf( str, sizeof(str), "0x%.8x", m_homeId );
	driverElement->SetAttribute( "home_id", str );

	snprintf( str, sizeof(str), "%d", m_nodeId );
	driverElement->SetAttribute( "node_id", str );

	snprintf( str, sizeof(str), "%d", m_initCaps );
	driverElement->SetAttribute( "api_capabilities", str );

	snprintf( str, sizeof(str), "%d", m_controllerCaps );
	driverElement->SetAttribute( "controller_capabilities", str );

	snprintf( str, sizeof(str), "%d", m_pollInterval );
	driverElement->SetAttribute( "poll_interval", str );

	LockNodes();
	for( int i=0; i<256; ++i )
	{
		if( m_nodes[i] )
		{
			m_nodes[i]->WriteXML( driverElement );
		}
	}
	ReleaseNodes();

	string userPath;
	Options::Get()->GetOptionAsString( "UserPath", &userPath );

	snprintf( str, sizeof(str), "zwcfg_0x%08x.xml", m_homeId );
	string filename =  userPath + string(str);

	doc.SaveFile( filename.c_str() );
}

//-----------------------------------------------------------------------------
//	Controller
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// <Driver::GetNodeUnsafe>
// Returns a pointer to the requested node without locking.
// Only to be used by main thread code.
//-----------------------------------------------------------------------------
Node* Driver::GetNodeUnsafe
(
	uint8 _nodeId
)
{
	if( Node* node = m_nodes[_nodeId] )
	{
		return node;
	}
	return NULL;
}

//-----------------------------------------------------------------------------
// <Driver::GetNode>
// Locks the nodes and returns a pointer to the requested one
//-----------------------------------------------------------------------------
Node* Driver::GetNode
(
	uint8 _nodeId
)
{
	LockNodes();
	if( Node* node = m_nodes[_nodeId] )
	{
		return node;
	}

	ReleaseNodes();
	return NULL;
}

//-----------------------------------------------------------------------------
// <Driver::LockNodes>
// Lock the nodes so that no other thread can modify them
//-----------------------------------------------------------------------------
void Driver::LockNodes
(
)
{
	m_nodeMutex->Lock();
}

//-----------------------------------------------------------------------------
// <Driver::ReleaseNodes>
// Unlock the nodes so that other threads can modify them
//-----------------------------------------------------------------------------
void Driver::ReleaseNodes
(
)
{
	m_nodeMutex->Unlock();
}

//-----------------------------------------------------------------------------
//	Sending Z-Wave messages
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// <Driver::SendQueryStageComplete>
// Queue an item on the query queue that indicates a stage is complete
//-----------------------------------------------------------------------------
void Driver::SendQueryStageComplete
( 
	uint8 const _nodeId,
	Node::QueryStage const _stage,
	MsgQueue const _queue 
)
{
	MsgQueueItem item;
	item.m_command = MsgQueueCmd_QueryStageComplete;
	item.m_nodeId = _nodeId;
	item.m_queryStage = _stage;

	if( Node* node = GetNode( _nodeId ) )
	{
		if( !node->IsListeningDevice() )
		{
			if( WakeUp* wakeUp = static_cast<WakeUp*>( node->GetCommandClass( WakeUp::StaticGetCommandClassId() ) ) )
			{
				if( !wakeUp->IsAwake() )
				{
					// If the message is for a sleeping node, we queue it in the node itself.
					Log::Write( "" );
					Log::Write( "Queuing Wake-Up Command: Query Stage Complete (%s)", node->GetQueryStageName( _stage ).c_str() );
					wakeUp->QueueMsg( item );
					ReleaseNodes();
					return;
				}
			}
		}

		// Non-sleeping node 
		Log::Write( "Queuing Command: Query Stage Complete (%s)", node->GetQueryStageName( _stage ).c_str() );
		m_sendMutex->Lock();
		m_msgQueue[MsgQueue_Query].push_back( item );
		m_queueEvent[MsgQueue_Query]->Set();
		m_sendMutex->Unlock();

		ReleaseNodes();
	}
}

//-----------------------------------------------------------------------------
// <Driver::SendMsg>
// Queue a message to be sent to the Z-Wave PC Interface
//-----------------------------------------------------------------------------
void Driver::SendMsg
( 
	Msg* _msg,
	MsgQueue const _queue 
)
{
	_msg->Finalize();

	MsgQueueItem item;
	item.m_command = MsgQueueCmd_SendMsg;
	item.m_msg = _msg;

	// If the message is for a sleeping node, we queue it in the node itself.
	if( Node* node = GetNode(_msg->GetTargetNodeId()) )
	{
		if( !node->IsListeningDevice() )
		{
			if( WakeUp* wakeUp = static_cast<WakeUp*>( node->GetCommandClass( WakeUp::StaticGetCommandClassId() ) ) )
			{
				if( !wakeUp->IsAwake() )
				{
					Log::Write( "" );
					Log::Write( "Queuing Wake-Up Command: %s", _msg->GetAsString().c_str() );
					wakeUp->QueueMsg( item );
					ReleaseNodes();
					return;
				}
			}
		}

		ReleaseNodes();
	}

	Log::Write( "Queuing command: %s", _msg->GetAsString().c_str() );
	m_sendMutex->Lock();
	m_msgQueue[_queue].push_back( item );
	m_queueEvent[_queue]->Set();
	m_sendMutex->Unlock();
}

//-----------------------------------------------------------------------------
// <Driver::WriteNextMsg>
// Transmit a queued message to the Z-Wave controller
//-----------------------------------------------------------------------------
bool Driver::WriteNextMsg
(
	MsgQueue const _queue 
)
{
	// There are messages to send, so get the one at the front of the queue
	m_sendMutex->Lock();
	MsgQueueItem const& item = m_msgQueue[_queue].front();
	
	if( MsgQueueCmd_SendMsg == item.m_command )
	{
		// Send a message 
		m_currentMsg = item.m_msg;
		m_msgQueue[_queue].pop_front();
		if( m_msgQueue[_queue].empty() )
		{
			m_queueEvent[_queue]->Reset();
		}
		m_sendMutex->Unlock();
		return WriteMsg();
	}
	
	if( MsgQueueCmd_QueryStageComplete == item.m_command )
	{
		// Move to the next query stage
		m_currentMsg = NULL;
		Node::QueryStage stage = item.m_queryStage;
		m_msgQueue[_queue].pop_front();
		if( m_msgQueue[_queue].empty() )
		{
			m_queueEvent[_queue]->Reset();
		}
		m_sendMutex->Unlock();

		Node* node = GetNodeUnsafe( item.m_nodeId );
		if( node != NULL )
		{	
			Log::Write( "Node %d: Query Stage Complete (%s)", node->GetNodeId(), node->GetQueryStageName( stage ).c_str() );
			node->QueryStageComplete( stage );
			node->AdvanceQueries();
			return true;
		}
	}

	return false;
}

//-----------------------------------------------------------------------------
// <Driver::WriteMsg>
// Transmit the current message to the Z-Wave controller
//-----------------------------------------------------------------------------
bool Driver::WriteMsg
(
)
{
	if( !m_currentMsg )
	{
		assert(0);
		return false;
	}

	uint8 attempts = m_currentMsg->GetSendAttempts();
	if( attempts >= MAX_TRIES )
	{
		// That's it - already tried to send MAX_TRIES times.
		Log::Write( "ERROR: Dropping command, expected response not received after %d attempt(s)", MAX_TRIES );
		delete m_currentMsg;
		m_currentMsg = NULL;

		m_expectedCallbackId = 0;
		m_expectedCommandClassId = 0;
		m_expectedNodeId = 0;
		m_expectedReply = 0;
		m_waitingForAck = false;
		return false;
	}

	m_currentMsg->SetSendAttempts( attempts + 1 );
	m_expectedCallbackId = m_currentMsg->GetCallbackId();
	m_expectedCommandClassId = m_currentMsg->GetExpectedCommandClassId();
	m_expectedNodeId = m_currentMsg->GetTargetNodeId();
	m_expectedReply = m_currentMsg->GetExpectedReply();
	m_waitingForAck = true;

	Log::Write( "" );
	Log::Write( "Sending command (Callback ID=0x%.2x, Expected Reply=0x%.2x) - %s", m_currentMsg->GetCallbackId(), m_currentMsg->GetExpectedReply(), m_currentMsg->GetAsString().c_str() );
			
	m_controller->Write( m_currentMsg->GetBuffer(), m_currentMsg->GetLength() );
	m_writeCnt++;
	
	uint8 nodeId = m_currentMsg->GetTargetNodeId();
	if( nodeId == 0xff )
	{
		m_controllerWriteCnt++;
	}
	else
	{
		Node* node = GetNodeUnsafe( nodeId );
		if( node != NULL )
		{
			node->m_writeCnt++;
		}
	}

	return true;
}

//-----------------------------------------------------------------------------
// <Driver::RemoveCurrentMsg>
// Delete the current message
//-----------------------------------------------------------------------------
void Driver::RemoveCurrentMsg
(
)
{
	delete m_currentMsg;
	m_currentMsg = NULL;

	m_expectedCallbackId = 0;
	m_expectedCommandClassId = 0;
	m_expectedNodeId = 0;
	m_expectedReply = 0;
	m_waitingForAck = false;
}

//-----------------------------------------------------------------------------
// <Driver::MoveMessagesToWakeUpQueue>
// Move messages for a sleeping device to its wake-up queue
//-----------------------------------------------------------------------------
bool Driver::MoveMessagesToWakeUpQueue
(
	uint8 const _targetNodeId
)
{
	// If the target node is one that goes to sleep, transfer
	// all messages for it to its Wake-Up queue.
	if( Node* node = GetNodeUnsafe(_targetNodeId) )
	{
		// Exclude controllers from battery check
		if( !node->IsListeningDevice() && !node->IsFrequentListeningDevice() && !node->IsController() )
		{
			if( WakeUp* wakeUp = static_cast<WakeUp*>( node->GetCommandClass( WakeUp::StaticGetCommandClassId() ) ) )
			{
				// Mark the node as asleep
				wakeUp->SetAwake( false );

				// Move all messages for this node to the wake-up queue									
				m_sendMutex->Lock();

				// Try the current message first
				if( m_currentMsg )
				{
					if( _targetNodeId == m_currentMsg->GetTargetNodeId() )
					{
						// This message is for the unresponsive node
						// We do not move any "Wake Up No More Information"
						// commands to the pending queue.
						if( !m_currentMsg->IsWakeUpNoMoreInformationCommand() )
						{
							Log::Write( "Node not responding - moving message to Wake-Up queue: %s", m_currentMsg->GetAsString().c_str() );
							MsgQueueItem item;
							item.m_command = MsgQueueCmd_SendMsg;
							item.m_msg = m_currentMsg;
							wakeUp->QueueMsg( item );
						}
						else
						{
							delete m_currentMsg;
						}
					
						m_currentMsg = NULL;
						m_expectedCallbackId = 0;
						m_expectedCommandClassId = 0;
						m_expectedNodeId = 0;
						m_expectedReply = 0;
						m_waitingForAck = false;
					}
				}

				// Now the message queues
				for( int i=0; i<MsgQueue_Count; ++i )
				{
					list<MsgQueueItem>::iterator it = m_msgQueue[i].begin();
					while( it != m_msgQueue[i].end() )
					{
						bool remove = false;
						MsgQueueItem const& item = *it;
						if( MsgQueueCmd_SendMsg == item.m_command )
						{
							if( _targetNodeId == item.m_msg->GetTargetNodeId() )
							{
								// This message is for the unresponsive node
								// We do not move any "Wake Up No More Information"
								// commands to the pending queue.
								if( !item.m_msg->IsWakeUpNoMoreInformationCommand() )
								{
									Log::Write( "Node not responding - moving message to Wake-Up queue: %s", item.m_msg->GetAsString().c_str() );
									wakeUp->QueueMsg( item );
								}
								else
								{
									delete item.m_msg;
								}
								remove = true;
							}
						}
						if( MsgQueueCmd_QueryStageComplete == item.m_command )
						{
							if( _targetNodeId == item.m_nodeId )
							{
								Log::Write( "Node not responding - moving QueryStageComplete command to Wake-Up queue" );
								wakeUp->QueueMsg( item );
								remove = true;
							}
						}

						if( remove )
						{
							it = m_msgQueue[i].erase( it );
						}
						else
						{
							++it;
						}
					}

					// If the queue is now empty, we need to clear its event
					if( m_msgQueue[i].empty() )
					{
						m_queueEvent[i]->Reset();
					}
				}

				m_sendMutex->Unlock();
				
				// Move completed successfully
				return true;
			}
		}
	}

	// Failed to move messages
	return false;
}

//-----------------------------------------------------------------------------
// <Driver::CheckCompletedNodeQueries>
// Identify controller (as opposed to node) commands...especially blocking ones
//-----------------------------------------------------------------------------
void Driver::CheckCompletedNodeQueries
(
)
{
	if( !m_allNodesQueried )
	{
		bool all = true;
		bool sleepingOnly = true;

		LockNodes();
		for( int i=0; i<256; ++i )
		{
			if( m_nodes[i] )
			{
				if( m_nodes[i]->GetCurrentQueryStage() != Node::QueryStage_Complete )
				{
					all = false;
					if( m_nodes[i]->IsListeningDevice() )
					{
						sleepingOnly = false;
					}
				}
			}
		}
		ReleaseNodes();

		if( all )
		{
			// no sleeping nodes, no more nodes in the queue, so...All done
			Log::Write( "Node query processing complete." );
			Notification* notification = new Notification( Notification::Type_AllNodesQueried );
			notification->SetHomeAndNodeIds( m_homeId, 0xff );
			QueueNotification( notification ); 
			m_awakeNodesQueried = true;
			m_allNodesQueried = true;
		}
		else if( sleepingOnly )
		{
			if (!m_awakeNodesQueried ) 
			{
				// only sleeping nodes remain, so signal awake nodes queried complete
				Log::Write( "Node query processing complete except for sleeping nodes." );
				Notification* notification = new Notification( Notification::Type_AwakeNodesQueried );
				notification->SetHomeAndNodeIds( m_homeId, 0xff );
				QueueNotification( notification ); 
				m_awakeNodesQueried = true;
			}
		}
	}
}

//-----------------------------------------------------------------------------
// <Driver::IsControllerCommand>
// Identify controller (as opposed to node) commands...especially blocking ones
//-----------------------------------------------------------------------------
bool Driver::IsControllerCommand
(
	const uint8 _command
)
{
	// ranges of commands are used to enhance performance
	// the commands identified as "Controller Commands" needs to be reviewed as we
	// understand the protocol better and implement handlers
	if( _command == FUNC_ID_SERIAL_API_SOFT_RESET ) // 0x08
		return true;
	if( ( _command >= FUNC_ID_ZW_SET_DEFAULT ) && // 0x42
		( _command <= FUNC_ID_ZW_REQUEST_NODE_NEIGHBOR_UPDATE ) ) // 0x48
		return true;
	if( ( _command >= FUNC_ID_ZW_ADD_NODE_TO_NETWORK ) && // 0x4a
		( _command <= FUNC_ID_ZW_GET_SUC_NODE_ID ) )   // 0x56
		return true;
	if( ( _command >= FUNC_ID_ZW_REMOVE_FAILED_NODE_ID ) && // 0x61
		( _command <= FUNC_ID_ZW_REPLACE_FAILED_NODE ) ) // 0x63
		return true;
	if( _command == FUNC_ID_ZW_GET_ROUTING_INFO ) // 0x80
		return true;

	return false;
}

//-----------------------------------------------------------------------------
//	Receiving Z-Wave messages
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// <Driver::ReadMsg>
// Read data from the serial port
//-----------------------------------------------------------------------------
bool Driver::ReadMsg
(
)
{
	uint8 buffer[1024];

	if( !m_controller->Read( buffer, 1 ) )
	{
		// Nothing to read
		return false;
	}

	switch( buffer[0] )
	{
		case SOF:
		{
			m_SOFCnt++;
			if( m_waitingForAck )
			{
				Log::Write( "Unsolicited message received while waiting for ACK." );
				m_ACKWaiting++;
			}

			// Read the length byte.  Keep trying until we get it.
			m_controller->SetSignalThreshold( 1 );
			if( Wait::Single( m_controller, 100 ) < 0 )
			{
				Log::Write( "100ms passed without finding the length byte...aborting frame read");
				m_readAborts++;
				break;
			}

			m_controller->Read( &buffer[1], 1 );
			m_controller->SetSignalThreshold( buffer[1] );
			if( Wait::Single( m_controller, 500 ) < 0 )
			{
				Log::Write( "500ms passed without reading the rest of the frame...aborting frame read" );
				m_readAborts++;
				m_controller->SetSignalThreshold( 1 );
				break;
			}
			
			m_controller->Read( &buffer[2], buffer[1] );
			m_controller->SetSignalThreshold( 1 );

			uint32 length = buffer[1] + 2;

			// Log the data
			string str = "";
			for( uint32 i=0; i<length; ++i ) 
			{
				if( i )
				{
					str += ", ";
				}
			
				char byteStr[8];
				snprintf( byteStr, sizeof(byteStr), "0x%.2x", buffer[i] );
				str += byteStr;
			}
			Log::Write( "  Received: %s", str.c_str() );

			// Verify checksum
			uint8 checksum = 0xff;
			for( uint32 i=1; i<(length-1); ++i ) 
			{
				checksum ^= buffer[i];
			}
			
			if( buffer[length-1] == checksum )
			{
				// Checksum correct - send ACK
				uint8 ack = ACK;
				m_controller->Write( &ack, 1 );
				m_readCnt++;
			
				// Process the received message
				ProcessMsg( &buffer[2] );
			}
			else
			{
				Log::Write( "Checksum incorrect - sending NAK" );
				m_badChecksum++;
				uint8 nak = NAK;
				m_controller->Write( &nak, 1 );
			}
			break;
		}
			
		case CAN:
		{
			Log::Write( "CAN received...triggering resend" );
			m_CANCnt++;
			WriteMsg();
			break;
		}
		
		case NAK:
		{
			Log::Write( "NAK received...triggering resend" );
			m_NAKCnt++;
			WriteMsg();
			break;
		}

		case ACK:
		{
			Log::Write( "  ACK received CallbackId 0x%.2x Reply 0x%.2x", m_expectedCallbackId, m_expectedReply );
			m_ACKCnt++;
			
			m_waitingForAck = false;		
			if( ( 0 == m_expectedCallbackId ) && ( 0 == m_expectedReply ) )
			{
				// Remove the message from the queue, now that it has been acknowledged.
				RemoveCurrentMsg();
			}
			break;
		}
		
		default:
		{
			Log::Write( "ERROR! Out of frame flow! (0x%.2x).  Sending NAK.", buffer[0] );
			m_OOFCnt++;
			uint8 nak = NAK;
			m_controller->Write( &nak, 1 );
			break;
		}
	}

	return true;
}

//-----------------------------------------------------------------------------
// <Driver::ProcessMsg>
// Process data received from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::ProcessMsg
(
	uint8* _data
)
{
	bool handleCallback = true;

	if( RESPONSE == _data[0] )
	{
		switch( _data[1] )
		{
			case FUNC_ID_SERIAL_API_GET_INIT_DATA:
			{
				Log::Write( "" );
				HandleSerialAPIGetInitDataResponse( _data );
				break;
			}
			case FUNC_ID_ZW_GET_CONTROLLER_CAPABILITIES:
			{
				Log::Write( "" );
				HandleGetControllerCapabilitiesResponse( _data );
				break;
			}
			case FUNC_ID_SERIAL_API_GET_CAPABILITIES:
			{
				Log::Write( "" );
				HandleGetSerialAPICapabilitiesResponse( _data );
				break;
			}
			case FUNC_ID_ZW_SEND_DATA:
			{
				HandleSendDataResponse( _data, false );
				handleCallback = false;			// Skip the callback handling - a subsequent FUNC_ID_ZW_SEND_DATA request will deal with that
				break;
			}
			case FUNC_ID_ZW_GET_VERSION:
			{
				Log::Write( "" );
				HandleGetVersionResponse( _data );
				break;
			}
			case FUNC_ID_ZW_MEMORY_GET_ID:
			{
				Log::Write( "" );
				HandleMemoryGetIdResponse( _data );
				break;
			}
			case FUNC_ID_ZW_GET_NODE_PROTOCOL_INFO:
			{
				Log::Write( "" );
				HandleGetNodeProtocolInfoResponse( _data );
				break;
			}
			case FUNC_ID_ZW_REPLICATION_SEND_DATA:
			{
				HandleSendDataResponse( _data, true );
				handleCallback = false;			// Skip the callback handling - a subsequent FUNC_ID_ZW_REPLICATION_SEND_DATA request will deal with that
				break;
			}
			case FUNC_ID_ZW_ASSIGN_RETURN_ROUTE:
			{
				Log::Write( "" );
				if( !HandleAssignReturnRouteResponse( _data ) )
				{
					m_expectedCallbackId = _data[2];	// The callback message won't be coming, so we force the transaction to complete
					m_expectedReply = 0;
					m_expectedCommandClassId = 0;
					m_expectedNodeId = 0;
				}
				break;
			}
			case FUNC_ID_ZW_DELETE_RETURN_ROUTE:
			{
				Log::Write( "" );
				if( !HandleDeleteReturnRouteResponse( _data ) )
				{
					m_expectedCallbackId = _data[2];	// The callback message won't be coming, so we force the transaction to complete
					m_expectedReply = 0;
					m_expectedCommandClassId = 0;
					m_expectedNodeId = 0;
				}
				break;
			}
			case FUNC_ID_ZW_ENABLE_SUC:
			{
				Log::Write( "" );
				HandleEnableSUCResponse( _data );
				break;
			}
			case FUNC_ID_ZW_REQUEST_NETWORK_UPDATE:
			{
				Log::Write( "" );
				if( !HandleNetworkUpdateResponse( _data ) )
				{
					m_expectedCallbackId = _data[2];	// The callback message won't be coming, so we force the transaction to complete
					m_expectedReply = 0;
					m_expectedCommandClassId = 0;
					m_expectedNodeId = 0;
				}
				break;
			}
			case FUNC_ID_ZW_SET_SUC_NODE_ID:
			{
				Log::Write( "" );
				HandleSetSUCNodeIdResponse( _data );
				break;
			}
			case FUNC_ID_ZW_GET_SUC_NODE_ID:
			{
				Log::Write( "" );
				HandleGetSUCNodeIdResponse( _data );
				break;
			}
			case FUNC_ID_ZW_REQUEST_NODE_INFO:
			{
				Log::Write( "" );
				Log::Write("Received reply to FUNC_ID_ZW_REQUEST_NODE_INFO" );
				break;
			}
			case FUNC_ID_ZW_REMOVE_FAILED_NODE_ID:
			{
				Log::Write( "" );
				if( !HandleRemoveFailedNodeResponse( _data ) )
				{
					m_expectedCallbackId = _data[2];	// The callback message won't be coming, so we force the transaction to complete
					m_expectedReply = 0;
					m_expectedCommandClassId = 0;
					m_expectedNodeId = 0;
				}
				break;
			}
			case FUNC_ID_ZW_IS_FAILED_NODE_ID:
			{
				Log::Write( "" );
				HandleIsFailedNodeResponse( _data );
				break;
			}
			case FUNC_ID_ZW_REPLACE_FAILED_NODE:
			{
				Log::Write( "" );
				if( !HandleReplaceFailedNodeResponse( _data ) )
				{
					m_expectedCallbackId = _data[2];	// The callback message won't be coming, so we force the transaction to complete
					m_expectedReply = 0;
					m_expectedCommandClassId = 0;
					m_expectedNodeId = 0;
				}
				break;
			}
			case FUNC_ID_ZW_GET_ROUTING_INFO:
			{
				Log::Write( "" );
				HandleGetRoutingInfoResponse( _data );
				break;
			}
			case FUNC_ID_ZW_R_F_POWER_LEVEL_SET:
			{
				Log::Write( "" );
				HandleRfPowerLevelSetResponse( _data );
                break;
			}
			case FUNC_ID_ZW_READ_MEMORY:
			{
				Log::Write( "" );
				HandleReadMemoryResponse( _data );
                break;
			}
			case FUNC_ID_SERIAL_API_SET_TIMEOUTS:
			{
				Log::Write( "" );
				HandleSerialApiSetTimeoutsResponse( _data );
				break;
			}
			case FUNC_ID_MEMORY_GET_BYTE:
			{
				Log::Write( "" );
				HandleMemoryGetByteResponse( _data );
				break;
			}
			default:
			{
				Log::Write( "" );
				Log::Write( "**TODO: handle response for 0x%.2x**", _data[1] );
				break;
			}
		}
	} 
	else if( REQUEST == _data[0] )
	{
		switch( _data[1] )
		{
			case FUNC_ID_APPLICATION_COMMAND_HANDLER:
			{
				Log::Write( "" );
				HandleApplicationCommandHandlerRequest( _data );
				break;
			}
			case FUNC_ID_ZW_SEND_DATA:
			{
				HandleSendDataRequest( _data, false );
				break;
			}
			case FUNC_ID_ZW_REPLICATION_COMMAND_COMPLETE:
			{
				if( m_controllerReplication )
				{
					Log::Write( "" );
					m_controllerReplication->SendNextData( m_controllerCommandNode );
				}
				break;
			}
			case FUNC_ID_ZW_REPLICATION_SEND_DATA:
			{
				HandleSendDataRequest( _data, true );
				break;
			}
			case FUNC_ID_ZW_ASSIGN_RETURN_ROUTE:
			{
				Log::Write( "" );
				HandleAssignReturnRouteRequest( _data );
				break;
			}
			case FUNC_ID_ZW_DELETE_RETURN_ROUTE:
			{
				Log::Write( "" );
				HandleDeleteReturnRouteRequest( _data );
				break;
			}
			case FUNC_ID_ZW_REQUEST_NODE_NEIGHBOR_UPDATE:
			{
				Log::Write( "" );
				HandleNodeNeighborUpdateRequest( _data );
				break;
			}
			case FUNC_ID_ZW_APPLICATION_UPDATE:
			{
				Log::Write( "" );
				handleCallback = !HandleApplicationUpdateRequest( _data );
				break;
			}
			case FUNC_ID_ZW_ADD_NODE_TO_NETWORK:
			{
				Log::Write( "" );
				HandleAddNodeToNetworkRequest( _data );
				break;
			}
			case FUNC_ID_ZW_REMOVE_NODE_FROM_NETWORK:
			{
				Log::Write( "" );
				HandleRemoveNodeFromNetworkRequest( _data );
				break;
			}
			case FUNC_ID_ZW_CREATE_NEW_PRIMARY:
			{
				Log::Write( "" );
				HandleCreateNewPrimaryRequest( _data );
				break;
			}
			case FUNC_ID_ZW_CONTROLLER_CHANGE:
			{
				Log::Write( "" );
				HandleControllerChangeRequest( _data );
				break;
			}
			case FUNC_ID_ZW_SET_LEARN_MODE:
			{
				Log::Write( "" );
				HandleSetLearnModeRequest( _data );
				break;
			}
			case FUNC_ID_ZW_REQUEST_NETWORK_UPDATE:
			{
				Log::Write( "" );
				HandleNetworkUpdateRequest( _data );
				break;
			}
			case FUNC_ID_ZW_REMOVE_FAILED_NODE_ID:
			{
				Log::Write( "" );
				HandleRemoveFailedNodeRequest( _data );
				break;
			}
			case FUNC_ID_ZW_REPLACE_FAILED_NODE:
			{
				Log::Write( "" );
				HandleReplaceFailedNodeRequest( _data );
				break;
			}
			case FUNC_ID_PROMISCUOUS_APPLICATION_COMMAND_HANDLER:
			{
				Log::Write( "" );
				HandlePromiscuousApplicationCommandHandlerRequest( _data );
				break;
			}
			default:
			{
				break;
			}	
		}
	}

	// Generic callback handling
	if( handleCallback )
	{
		if( ( m_expectedCallbackId || m_expectedReply ) )
		{
			if( m_expectedCallbackId )
			{
				if( m_expectedCallbackId == _data[2] )
				{
					Log::Write( "  Expected callbackId was received" );
					m_expectedCallbackId = 0;
				}
			}
			if( m_expectedReply )
			{
				if( m_expectedReply == _data[1] )
				{
					if( m_expectedCommandClassId && ( m_expectedReply == FUNC_ID_APPLICATION_COMMAND_HANDLER ) )
					{
						if( m_expectedCommandClassId == _data[5] && m_expectedNodeId == _data[3] )
						{
							Log::Write( "  Expected reply and command class was received" );
							m_expectedReply = 0;
							m_expectedCommandClassId = 0;
							m_expectedNodeId = 0;
						}
					}
					else
					{
						Log::Write( "  Expected reply was received" );
						m_expectedReply = 0;
					}
				}
			}

			if( !( m_expectedCallbackId || m_expectedReply ) )
			{
				Log::Write( "  Message transaction complete" );
				Log::Write( "" );
				delete m_currentMsg;
				m_currentMsg = NULL;

				if( m_notifytransactions )
				{
					Notification* notification = new Notification( Notification::Type_MsgComplete );
					notification->SetHomeAndNodeIds( m_homeId, 0xff );
					QueueNotification( notification ); 
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------
// <Driver::HandleGetVersionResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleGetVersionResponse
(
	uint8* _data
)
{
	m_libraryVersion = (char*)&_data[2];
	
	uint8 m_libraryType = _data[m_libraryVersion.size()+3];
	if( m_libraryType < 9 )
	{
		m_libraryTypeName = c_libraryTypeNames[m_libraryType];
	}
	Log::Write( "Received reply to FUNC_ID_ZW_GET_VERSION:" );
	Log::Write( "    %s library, version %s", m_libraryTypeName.c_str(), m_libraryVersion.c_str() );
}

//-----------------------------------------------------------------------------
// <Driver::HandleGetControllerCapabilitiesResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleGetControllerCapabilitiesResponse
(
	uint8* _data
)
{
	m_controllerCaps = _data[2];

	Log::Write( "Received reply to FUNC_ID_ZW_GET_CONTROLLER_CAPABILITIES:" );

	char str[256];
	if( m_controllerCaps & ControllerCaps_SIS )
	{
		Log::Write( "    There is a SUC ID Server (SIS) in this network." );
		snprintf( str, 256, "    The PC controller is an inclusion %s%s%s", 
			( m_controllerCaps & ControllerCaps_SUC ) ? " static update controller (SUC)" : " controller",
			( m_controllerCaps & ControllerCaps_OnOtherNetwork ) ? " which is using a Home ID from another network" : "",
			( m_controllerCaps & ControllerCaps_RealPrimary ) ? " and was the original primary before the SIS was added." : "." );
		Log::Write( str );

	}
	else
	{
		Log::Write( "    There is no SUC ID Server (SIS) in this network." );
		snprintf( str, 256, "    The PC controller is a %s%s%s", 
			( m_controllerCaps & ControllerCaps_Secondary ) ? "secondary" : "primary",
			( m_controllerCaps & ControllerCaps_SUC ) ? " static update controller (SUC)" : " controller",
			( m_controllerCaps & ControllerCaps_OnOtherNetwork ) ? " which is using a Home ID from another network." : "." );
		Log::Write( str );
	}
}

//-----------------------------------------------------------------------------
// <Driver::HandleGetSerialAPICapabilitiesResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleGetSerialAPICapabilitiesResponse
(
	uint8* _data
)
{
	Log::Write( "Received reply to FUNC_ID_SERIAL_API_GET_CAPABILITIES" );
	Log::Write( "    Application Version:  %d", _data[2] );
	Log::Write( "    Application Revision: %d", _data[3] );
	Log::Write( "    Manufacturer ID:      0x%.2x%.2x", _data[4], _data[5] );
	Log::Write( "    Product Type:         0x%.2x%.2x", _data[6], _data[7] );
	Log::Write( "    Product ID:           0x%.2x%.2x", _data[8], _data[9] );

	// _data[10] to _data[41] are a 256-bit bitmask with one bit set for 
	// each FUNC_ID_ method supported by the controller.
	// Bit 0 is FUNC_ID_ 1.  So FUNC_ID_SERIAL_API_GET_CAPABILITIES (0x07) will be bit 6 of the first byte.
	m_manufacturerId = (((uint16)_data[4])<<8) | (uint16)_data[5];
	m_productType = (((uint16)_data[6])<<8) | (uint16)_data[7];
	m_productId = (((uint16)_data[8])<<8) | (uint16)_data[9];
}

//-----------------------------------------------------------------------------
// <Driver::HandleEnableSUCResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleEnableSUCResponse
(
	uint8* _data
)
{
	Log::Write( "Received reply to Enable SUC." );
}

//-----------------------------------------------------------------------------
// <Driver::HandleNetworkUpdateResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
bool Driver::HandleNetworkUpdateResponse
(
	uint8* _data
)
{
	bool res = true;
	ControllerState state = ControllerState_InProgress;
	if( _data[2] )
	{
		Log::Write("Received reply to FUNC_ID_ZW_REQUEST_NETWORK_UPDATE - command in progress" );
	}
	else
	{
		// Failed
		Log::Write("Received reply to FUNC_ID_ZW_REQUEST_NETWORK_UPDATE - command failed" );
		state = ControllerState_Failed;
		m_controllerCommand = ControllerCommand_None;
		res = false;
	}

	if( m_controllerCallback )
	{
		m_controllerCallback( state, m_controllerCallbackContext );
	}
	return res; 
}

//-----------------------------------------------------------------------------
// <Driver::HandleSetSUCNodeIdResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleSetSUCNodeIdResponse
(
	uint8* _data
)
{
	Log::Write( "Received reply to SET_SUC_NODE_ID." );
}

//-----------------------------------------------------------------------------
// <Driver::HandleGetSUCNodeIdResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleGetSUCNodeIdResponse
(
	uint8* _data
)
{
	Log::Write( "Received reply to GET_SUC_NODE_ID.  Node ID = %d", _data[2] );

	if( _data[2] == 0)
	{
		Log::Write( "  No SUC, so we become SUC" );
		
		Msg* msg;

		msg = new Msg( "Enable SUC", m_nodeId, REQUEST, FUNC_ID_ZW_ENABLE_SUC, false );
		msg->Append( 1 );	
//		msg->Append( SUC_FUNC_BASIC_SUC );			// SUC
		msg->Append( SUC_FUNC_NODEID_SERVER );		// SIS
		SendMsg( msg, MsgQueue_Send ); 

		msg = new Msg( "Set SUC node ID", m_nodeId, REQUEST, FUNC_ID_ZW_SET_SUC_NODE_ID, false );
		msg->Append( m_nodeId );
		msg->Append( 1 );								// TRUE, we want to be SUC/SIS
		msg->Append( 0 );								// no low power
		msg->Append( SUC_FUNC_NODEID_SERVER );
		SendMsg( msg, MsgQueue_Send ); 
	}
}

//-----------------------------------------------------------------------------
// <Driver::HandleMemoryGetIdResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleMemoryGetIdResponse
(
	uint8* _data
)
{
	Log::Write( "Received reply to FUNC_ID_ZW_MEMORY_GET_ID. Home ID = 0x%02x%02x%02x%02x.  Our node ID = %d", _data[2], _data[3], _data[4], _data[5], _data[6] );
	m_homeId = (((uint32)_data[2])<<24) | (((uint32)_data[3])<<16) | (((uint32)_data[4])<<8) | ((uint32)_data[5]);
	m_nodeId = _data[6];
	m_controllerReplication = static_cast<ControllerReplication*>(ControllerReplication::Create( m_homeId, m_nodeId ));
}

//-----------------------------------------------------------------------------
// <Driver::HandleSerialAPIGetInitDataResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleSerialAPIGetInitDataResponse
(
	uint8* _data
)
{
	int32 i;

	if( !m_init )
	{
		// Mark the driver as ready (we have to do this first or 
		// all the code handling notifications will go awry).
		Manager::Get()->SetDriverReady( this, true );

		// Read the config file first, to get the last known state
		ReadConfig();
	}

	Log::Write( "Received reply to FUNC_ID_SERIAL_API_GET_INIT_DATA:" );
	m_initVersion = _data[2];
	m_initCaps = _data[3];

	if( _data[4] == NUM_NODE_BITFIELD_BYTES )
	{
		for( i=0; i<NUM_NODE_BITFIELD_BYTES; ++i)
		{
			for( int32 j=0; j<8; ++j )
			{
				uint8 nodeId = (i*8)+j+1;
				if( _data[i+5] & (0x01 << j) )
				{					
					if( Node* node = GetNode( nodeId ) )
					{
						node = node;
						Log::Write( "    Node %d - Known", nodeId );
						if( !m_init )
						{
							// The node was read in from the config, so we 
							// only need to get its current state
							// node->SetQueryStage( Node::QueryStage_Associations );
						}

						ReleaseNodes();
					}
					else
					{
						// This node is new
						Log::Write( "    Node %.3d - New", nodeId );
						Notification* notification = new Notification( Notification::Type_NodeNew );
						notification->SetHomeAndNodeIds( m_homeId, nodeId );
						QueueNotification( notification ); 

						// Create the node and request its info
						InitNode( nodeId );		
					}
				}
				else
				{
					if( GetNode(nodeId) )
					{
						// This node no longer exists in the Z-Wave network
						Log::Write( "    Node %.3d: Removed", nodeId );
						Notification* notification = new Notification( Notification::Type_NodeRemoved );
						notification->SetHomeAndNodeIds( m_homeId, nodeId );
						QueueNotification( notification ); 

						delete m_nodes[nodeId];
						m_nodes[nodeId] = NULL;
						ReleaseNodes();
					}
				}
			}
		}
	}

	m_init = true;
}

//-----------------------------------------------------------------------------
// <Driver::HandleGetNodeProtocolInfoResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleGetNodeProtocolInfoResponse
(
	uint8* _data
)
{
	// The node that the protocol info response is for is not included in the message.
	// We have to assume that the node is the same one as in the most recent request.
	if( !m_currentMsg )
	{
		Log::Write("ERROR: Received unexpected FUNC_ID_ZW_GET_NODE_PROTOCOL_INFO message - ignoring.");
		return;
	}

	uint8 nodeId = m_currentMsg->GetTargetNodeId();
	Log::Write("Received reply to FUNC_ID_ZW_GET_NODE_PROTOCOL_INFO for node %d", nodeId );

	// Update the node with the protocol info
	if( Node* node = GetNodeUnsafe( nodeId ) )
	{
		node->UpdateProtocolInfo( &_data[2] );
	}
}

//-----------------------------------------------------------------------------
// <Driver::HandleAssignReturnRouteResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
bool Driver::HandleAssignReturnRouteResponse
(
	uint8* _data
)
{
	bool res = true;
	ControllerState state = ControllerState_InProgress;
	if( _data[2] )
	{
		Log::Write("Received reply to FUNC_ID_ZW_ASSIGN_RETURN_ROUTE - command in progress" );
	}
	else
	{
		// Failed
		Log::Write("Received reply to FUNC_ID_ZW_ASSIGN_RETURN_ROUTE - command failed" );
		state = ControllerState_Failed;
		m_controllerCommand = ControllerCommand_None;
		res = false;
	}

	if( m_controllerCallback )
	{
		m_controllerCallback( state, m_controllerCallbackContext );
	}
	return res; 
}

//-----------------------------------------------------------------------------
// <Driver::HandleDeleteReturnRouteResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
bool Driver::HandleDeleteReturnRouteResponse
(
	uint8* _data
)
{
	bool res = true;
	ControllerState state = ControllerState_InProgress;
	if( _data[2] )
	{
		Log::Write("Received reply to FUNC_ID_ZW_DELETE_RETURN_ROUTE - command in progress" );
	}
	else
	{
		// Failed
		Log::Write("Received reply to FUNC_ID_ZW_DELETE_RETURN_ROUTE - command failed" );
		state = ControllerState_Failed;
		m_controllerCommand = ControllerCommand_None;
		res = false;
	}

	if( m_controllerCallback )
	{
		m_controllerCallback( state, m_controllerCallbackContext );
	}
	return res; 
}

//-----------------------------------------------------------------------------
// <Driver::HandleRemoveFailedNodeResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
bool Driver::HandleRemoveFailedNodeResponse
(
	uint8* _data
)
{
	bool res = true;
	ControllerState state = ControllerState_InProgress;
	if( _data[2] )
	{
		// Failed
		Log::Write("Received reply to FUNC_ID_ZW_REMOVE_FAILED_NODE_ID - command failed" );
		state = ControllerState_Failed;
		m_controllerCommand = ControllerCommand_None;
		res = false;
	}
	else
	{
		Log::Write("Received reply to FUNC_ID_ZW_REMOVE_FAILED_NODE_ID - command in progress" );
	}

	if( m_controllerCallback )
	{
		m_controllerCallback( state, m_controllerCallbackContext );
	}
	return res; 
}

//-----------------------------------------------------------------------------
// <Driver::HandleIsFailedNodeResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleIsFailedNodeResponse
(
	uint8* _data
)
{
	Log::Write("Received reply to FUNC_ID_ZW_IS_FAILED_NODE_ID - node %d has %s", m_controllerCommandNode, _data[2] ? "failed" : "not failed" );
	if( m_controllerCallback )
	{
		m_controllerCallback( _data[2] ? ControllerState_NodeFailed : ControllerState_NodeOK, m_controllerCallbackContext );
	}

	m_controllerCommand = ControllerCommand_None;
}

//-----------------------------------------------------------------------------
// <Driver::HandleReplaceFailedNodeResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
bool Driver::HandleReplaceFailedNodeResponse
(
	uint8* _data
)
{
	bool res = true;
	ControllerState state = ControllerState_InProgress;
	if( _data[2] )
	{
		// Command failed
		Log::Write("Received reply to FUNC_ID_ZW_REPLACE_FAILED_NODE - command failed" );
		state = ControllerState_Failed;
		m_controllerCommand = ControllerCommand_None;
		res = false;
	}
	else
	{
		Log::Write("Received reply to FUNC_ID_ZW_REPLACE_FAILED_NODE - command in progress" );
	}

	if( m_controllerCallback )
	{
		m_controllerCallback( state, m_controllerCallbackContext );
	}
	return res;
}

//-----------------------------------------------------------------------------
// <Driver::HandleSendDataResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleSendDataResponse
(
	uint8* _data,
	bool _replication
)
{
	if( _data[2] )
	{
		Log::Write( "  %s delivered to Z-Wave stack", _replication ? "ZW_REPLICATION_SEND_DATA" : "ZW_SEND_DATA" );
	}
	else
	{
		Log::Write("ERROR: %s could not be delivered to Z-Wave stack", _replication ? "ZW_REPLICATION_SEND_DATA" : "ZW_SEND_DATA" );
	}
}

//-----------------------------------------------------------------------------
// <Driver::HandleGetRoutingInfoResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleGetRoutingInfoResponse
(
	uint8* _data
)
{
	Log::Write("Received reply to FUNC_ID_ZW_GET_ROUTING_INFO" );

	if( Node* node = GetNode( m_controllerCommandNode ) )
	{
		// copy the 29-byte bitmap received (29*8=232 possible nodes) into this node's neighbors member variable
		memcpy( node->m_neighbors, &_data[2], 29 );
		ReleaseNodes();
		Log::Write( "    Neighbors of this node are:" );
		bool bNeighbors = false;
		for( int by=0; by<29; by++ )
		{
			for( int bi=0; bi<8; bi++ )
			{
				if( (_data[2+by] & (0x01<<bi)) )
				{
					Log::Write( "    Node %d", (by<<3)+bi+1 );
					bNeighbors = true;
				}
			}
		}
		
		if( !bNeighbors )
		{
			Log::Write( "    (none reported)" );
		}
	}


	if( m_controllerCallback )
	{
		m_controllerCallback( ControllerState_Completed, m_controllerCallbackContext );
	}
	m_controllerCommand = ControllerCommand_None;
}

//-----------------------------------------------------------------------------
// <Driver::HandleSendDataRequest>
// Process a request from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleSendDataRequest
(
	uint8* _data,
	bool _replication
)
{
	Log::Write( "  %s Request with callback ID 0x%.2x received (expected 0x%.2x)", _replication ? "ZW_REPLICATION_SEND_DATA" : "ZW_SEND_DATA", _data[2], m_expectedCallbackId );

	if( _data[2] != m_expectedCallbackId )
	{
		// Wrong callback ID
		Log::Write( "ERROR: Callback ID is invalid" );
	}
	else 
	{
		// Callback ID matches our expectation
		if( _data[3] & TRANSMIT_COMPLETE_NOROUTE )
		{
			Log::Write( "Error: %s failed.  No route available.", _replication ? "ZW_REPLICATION_SEND_DATA" : "ZW_SEND_DATA" );
			RemoveCurrentMsg();
		}
		else if( _data[3] & TRANSMIT_COMPLETE_NO_ACK )
		{
			Log::Write( "Error: %s failed. No ACK received - device may be asleep.", _replication ? "ZW_REPLICATION_SEND_DATA" : "ZW_SEND_DATA" );
			if( m_currentMsg )
			{
				if( !_replication )
				{
					// In case the failure is due to the target being a sleeping node, we 
					// first try to move its pending messages to its wake-up queue.
					if( MoveMessagesToWakeUpQueue( m_currentMsg->GetTargetNodeId() ) )
					{
						return;
					}

					Log::Write( "  Device is not a sleeping node - retrying the send." );
				}
			}
		}
		else if( _data[3] & TRANSMIT_COMPLETE_FAIL )
		{
			Log::Write( "Error: %s failed. Network is busy.", _replication ? "ZW_REPLICATION_SEND_DATA" : "ZW_SEND_DATA" );
		}
		else
		{
			// Command reception acknowledged by node
			m_expectedCallbackId = 0;
		}
	}
}

//-----------------------------------------------------------------------------
// <Driver::HandleNetworkUpdateRequest>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleNetworkUpdateRequest
(
	uint8* _data
)
{
	ControllerState state = ControllerState_Failed;
	switch( _data[3] )
	{
		case SUC_UPDATE_DONE:
		{
			Log::Write("Received reply to FUNC_ID_ZW_REQUEST_NETWORK_UPDATE: Success" );
			state = ControllerState_Completed;
			break;
		}
		case SUC_UPDATE_ABORT:
		{
			Log::Write("Received reply to FUNC_ID_ZW_REQUEST_NETWORK_UPDATE: Failed - Error. Process aborted." );
			break;
		}
		case SUC_UPDATE_WAIT:
		{
			Log::Write("Received reply to FUNC_ID_ZW_REQUEST_NETWORK_UPDATE: Failed - SUC is busy." );
			break;
		}
		case SUC_UPDATE_DISABLED:
		{
			Log::Write("Received reply to FUNC_ID_ZW_REQUEST_NETWORK_UPDATE: Failed - SUC is disabled." );
			break;
		}
		case SUC_UPDATE_OVERFLOW:
		{
			Log::Write("Received reply to FUNC_ID_ZW_REQUEST_NETWORK_UPDATE: Failed - Overflow. Full replication required." );
			break;
		}
		default:
		{
		}
	}

	if( m_controllerCallback )
	{
		m_controllerCallback( state, m_controllerCallbackContext );
	}
	m_controllerCommand = ControllerCommand_None;
}

//-----------------------------------------------------------------------------
// <Driver::HandleAddNodeToNetworkRequest>
// Process a request from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleAddNodeToNetworkRequest
(
	uint8* _data
)
{
	Log::Write( "FUNC_ID_ZW_ADD_NODE_TO_NETWORK:" );
	CommonAddNodeStatusRequestHandler( FUNC_ID_ZW_ADD_NODE_TO_NETWORK, _data );
}

//-----------------------------------------------------------------------------
// <Driver::HandleRemoveNodeFromNetworkRequest>
// Process a request from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleRemoveNodeFromNetworkRequest
(
	uint8* _data
)
{
	Log::Write( "FUNC_ID_ZW_REMOVE_NODE_FROM_NETWORK:" );
	
	switch( _data[3] ) 
	{
		case REMOVE_NODE_STATUS_LEARN_READY:
		{
			Log::Write( "REMOVE_NODE_STATUS_LEARN_READY" );
			m_controllerCommandNode = 0;
			if( m_controllerCallback )
			{
				m_controllerCallback( ControllerState_Waiting, m_controllerCallbackContext );
			}
			break;
		}
		case REMOVE_NODE_STATUS_NODE_FOUND:
		{
			Log::Write( "REMOVE_NODE_STATUS_NODE_FOUND" );
			if( m_controllerCallback )
			{
				m_controllerCallback( ControllerState_InProgress, m_controllerCallbackContext );
			}
			break;
		}
		case REMOVE_NODE_STATUS_REMOVING_SLAVE:
		{
			Log::Write( "REMOVE_NODE_STATUS_REMOVING_SLAVE" );
			Log::Write( "Removing node ID %d", _data[4] );
			m_controllerCommandNode = _data[4];
			break;
		}
		case REMOVE_NODE_STATUS_REMOVING_CONTROLLER:
		{
			Log::Write( "REMOVE_NODE_STATUS_REMOVING_CONTROLLER" );
			m_controllerCommandNode = _data[4];
			if( m_controllerCommandNode == 0 ) // Some controllers don't return node number
			{
				if( _data[5] >= 3 )
				{
					for( int i=0; i<256; i++ )
					{
						if( m_nodes[i] == NULL )
						{
							continue;
						}
						// Ignore primary controller
						if( m_nodes[i]->m_nodeId == m_nodeId )
						{
							continue;
						}
						// See if we can match another way
						if( m_nodes[i]->m_basic == _data[6] &&
						    m_nodes[i]->m_generic == _data[7] &&
						    m_nodes[i]->m_specific == _data[8] )
						{
							if( m_controllerCommandNode != 0 )
							{
								Log::Write( "Alternative controller lookup found more then one match. Using the first one found.");
							}
							else
							{
								m_controllerCommandNode = m_nodes[i]->m_nodeId;
							}
						}
					}
				}
				else
				{
					Log::Write( "Node is 0 but not enough data to perform alternative match.");
				}
			}
			else
			{
				m_controllerCommandNode = _data[4];
			}
			Log::Write( "Removing controller ID %d", m_controllerCommandNode );
			break;
		}
		case REMOVE_NODE_STATUS_DONE:
		{
			Log::Write( "REMOVE_NODE_STATUS_DONE" );
			
			if ( m_controllerCommandNode == 0 ) // never received "removing" update
			{
				if ( _data[4] != 0 ) // but message has the clue
					m_controllerCommandNode = _data[4];
			}

			if ( m_controllerCommandNode != 0 )
			{
				Notification* notification = new Notification( Notification::Type_NodeRemoved );
				notification->SetHomeAndNodeIds( m_homeId, m_controllerCommandNode );
				QueueNotification( notification ); 
			
				LockNodes();
				delete m_nodes[m_controllerCommandNode];
				m_nodes[m_controllerCommandNode] = NULL;
				ReleaseNodes();
			}

			if( m_controllerCallback )
			{
				m_controllerCallback( ControllerState_Completed, m_controllerCallbackContext );
			}
			m_controllerCommand = ControllerCommand_None;
			break;
		}
		case REMOVE_NODE_STATUS_FAILED:
		{
			Log::Write( "REMOVE_NODE_STATUS_FAILED" );
			if( m_controllerCallback )
			{
				m_controllerCallback( ControllerState_Failed, m_controllerCallbackContext );
			}
			m_controllerCommand = ControllerCommand_None;
			break;
		}
		default:
		{
			break;
		}
	}
}

//-----------------------------------------------------------------------------
// <Driver::HandleControllerChangeRequest>
// Process a request from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleControllerChangeRequest
(
	uint8* _data
)
{
	Log::Write( "FUNC_ID_ZW_CONTROLLER_CHANGE:" );
	CommonAddNodeStatusRequestHandler( FUNC_ID_ZW_CONTROLLER_CHANGE, _data );
}

//-----------------------------------------------------------------------------
// <Driver::HandleCreateNewPrimaryRequest>
// Process a request from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleCreateNewPrimaryRequest
(
	uint8* _data
)
{
	Log::Write( "FUNC_ID_ZW_CREATE_NEW_PRIMARY:" );
	CommonAddNodeStatusRequestHandler( FUNC_ID_ZW_CREATE_NEW_PRIMARY, _data );
}

//-----------------------------------------------------------------------------
// <Driver::HandleSetLearnModeRequest>
// Process a request from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleSetLearnModeRequest
(
	uint8* _data
)
{
	Log::Write( "FUNC_ID_ZW_SET_LEARN_MODE:" );
	
	switch( _data[3] ) 
	{
		case LEARN_MODE_STARTED:
		{
			Log::Write( "LEARN_MODE_STARTED" );
			if( m_controllerCallback )
			{
				m_controllerCallback( ControllerState_Waiting, m_controllerCallbackContext );
			}
			break;
		}
		case LEARN_MODE_DONE:
		{
			Log::Write( "LEARN_MODE_DONE" );
			if( m_controllerCallback )
			{
				m_controllerCallback( ControllerState_Completed, m_controllerCallbackContext );
			}
			m_controllerCommand = ControllerCommand_None;

			// Stop learn mode
			Msg* msg = new Msg( "End Learn Mode", 0xff, REQUEST, FUNC_ID_ZW_SET_LEARN_MODE, false, false );
			msg->Append( 0 );
			SendMsg( msg, MsgQueue_Command );

			// Rebuild all the node info.  Group and scene data that we stored 
			// during replication will be applied as we discover each node.
			InitAllNodes();
			break;
		}
		case LEARN_MODE_FAILED:
		{
			Log::Write( "LEARN_MODE_FAILED" );
			if( m_controllerCallback )
			{
				m_controllerCallback( ControllerState_Failed, m_controllerCallbackContext );
			}
			m_controllerCommand = ControllerCommand_None;

			// Controller change failed
			Msg* msg = new Msg(  "Controller change failed", 0xff, REQUEST, FUNC_ID_ZW_CONTROLLER_CHANGE, true, false );
			msg->Append( CONTROLLER_CHANGE_STOP_FAILED );
			SendMsg( msg, MsgQueue_Command );

			// Rebuild all the node info, since it may have been partially
			// updated by the failed command.  Group and scene data that we
			// stored during replication will be applied as we discover each node.
			InitAllNodes();
			break;
		}
		case LEARN_MODE_DELETED:
		{
			Log::Write( "LEARN_MODE_DELETED" );
			break;
		}
	}
}

//-----------------------------------------------------------------------------
// <Driver::HandleRemoveFailedNodeRequest>
// Process a request from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleRemoveFailedNodeRequest
(
	uint8* _data
)
{
	ControllerState state = ControllerState_Completed;
	switch( _data[3] )
	{
		case FAILED_NODE_OK:
		{
			Log::Write("Received reply to FUNC_ID_ZW_REMOVE_FAILED_NODE_ID - Node %d is OK, so command failed", m_controllerCommandNode );
			state = ControllerState_NodeOK;
			break;
		}
		case FAILED_NODE_REMOVED:
		{
			Log::Write("Received reply to FUNC_ID_ZW_REMOVE_FAILED_NODE_ID - node %d successfully moved to failed nodes list", m_controllerCommandNode );
			state = ControllerState_Completed;
			m_controllerCommand = ControllerCommand_None;
			break;
		}
		case FAILED_NODE_NOT_REMOVED:
		{
			Log::Write("Received reply to FUNC_ID_ZW_REMOVE_FAILED_NODE_ID - unable to move node %d to failed nodes list", m_controllerCommandNode );
			state = ControllerState_Failed;
			m_controllerCommand = ControllerCommand_None;
			break;
		}
	}

	if( m_controllerCallback )
	{
		m_controllerCallback( state, m_controllerCallbackContext );
	}
	m_controllerCommand = ControllerCommand_None;
}

//-----------------------------------------------------------------------------
// <Driver::HandleReplaceFailedNodeRequest>
// Process a request from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleReplaceFailedNodeRequest
(
	uint8* _data
)
{
	ControllerState state = ControllerState_Completed;
	switch( _data[3] )
	{
		case FAILED_NODE_OK:
		{
			Log::Write("Received reply to FUNC_ID_ZW_REPLACE_FAILED_NODE - Node %d is OK, so command failed", m_controllerCommandNode );
			state = ControllerState_NodeOK;
			m_controllerCommand = ControllerCommand_None;
			break;
		}
		case FAILED_NODE_REPLACE_WAITING:
		{
			Log::Write("Received reply to FUNC_ID_ZW_REPLACE_FAILED_NODE - Waiting for new node" );
			state = ControllerState_Waiting;
			break;
		}
		case FAILED_NODE_REPLACE_DONE:
		{
			Log::Write("Received reply to FUNC_ID_ZW_REPLACE_FAILED_NODE - node %d successfully replaced", m_controllerCommandNode );
			state = ControllerState_Completed;
			m_controllerCommand = ControllerCommand_None;

			// Request new node info for this device
			InitNode( m_controllerCommandNode );
			break;
		}
		case FAILED_NODE_REPLACE_FAILED:
		{
			Log::Write("Received reply to FUNC_ID_ZW_REPLACE_FAILED_NODE - node %d replacement failed", m_controllerCommandNode );
			state = ControllerState_Failed;
			m_controllerCommand = ControllerCommand_None;
			break;
		}
	}

	if( m_controllerCallback )
	{
		m_controllerCallback( state, m_controllerCallbackContext );
	}
}

//-----------------------------------------------------------------------------
// <Driver::HandleApplicationCommandHandlerRequest>
// Process a request from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleApplicationCommandHandlerRequest
(
	uint8* _data
)
{
	uint8 nodeId = _data[3];
	uint8 classId = _data[5];

	if( ApplicationStatus::StaticGetCommandClassId() == classId )
	{
		//TODO: Test this class function or implement
	}
	else if( ControllerReplication::StaticGetCommandClassId() == classId )
	{
		if( m_controllerReplication && ( ControllerCommand_ReceiveConfiguration == m_controllerCommand ) )
		{
			m_controllerReplication->HandleMsg( &_data[6], _data[4] );
			if( m_controllerCallback )
			{
				m_controllerCallback( ControllerState_InProgress, m_controllerCallbackContext );
			}		
		}
		else
		{

		}
	}
	else
	{
		// Allow the node to handle the message itself
		if( Node* node = GetNodeUnsafe( nodeId)  )
	 	{
			node->ApplicationCommandHandler( _data );
		}
	}
}

//-----------------------------------------------------------------------------
// <Driver::HandlePromiscuousApplicationCommandHandlerRequest>
// Process a request from the Z-Wave PC interface when in promiscuous mode.
//-----------------------------------------------------------------------------
void Driver::HandlePromiscuousApplicationCommandHandlerRequest
(
	uint8* _data
)
{
	//uint8 nodeId = _data[3];
	//uint8 len = _data[4];
	//uint8 classId = _data[5];
	//uint8 destNodeId = _data[5+len];
}

//-----------------------------------------------------------------------------
// <Driver::HandleAssignReturnRouteRequest>
// Process a request from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleAssignReturnRouteRequest
(
	uint8* _data
)
{
	if( _data[3] )
	{
		// Failed
		Log::Write("Received reply to FUNC_ID_ZW_ASSIGN_RETURN_ROUTE for node %d - FAILED", m_controllerCommandNode );
		if( m_controllerCallback )
		{
			m_controllerCallback( ControllerState_Failed, m_controllerCallbackContext );
		}
	}
	else
	{
		// Success
		Log::Write("Received reply to FUNC_ID_ZW_ASSIGN_RETURN_ROUTE for node %d - SUCCESS", m_controllerCommandNode );
		if( m_controllerCallback )
		{
			m_controllerCallback( ControllerState_Completed, m_controllerCallbackContext );
		}
	}

	m_controllerCommand = ControllerCommand_None;
}

//-----------------------------------------------------------------------------
// <Driver::HandleDeleteReturnRouteRequest>
// Process a request from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleDeleteReturnRouteRequest
(
	uint8* _data
)
{
	if( _data[3] )
	{
		// Failed
		Log::Write("Received reply to FUNC_ID_ZW_DELETE_RETURN_ROUTE for node %d - FAILED", m_controllerCommandNode );
		if( m_controllerCallback )
		{
			m_controllerCallback( ControllerState_Failed, m_controllerCallbackContext );
		}
	}
	else
	{
		// Success
		Log::Write("Received reply to FUNC_ID_ZW_DELETE_RETURN_ROUTE for node %d - SUCCESS", m_controllerCommandNode );
		if( m_controllerCallback )
		{
			m_controllerCallback( ControllerState_Completed, m_controllerCallbackContext );
		}
	}

	m_controllerCommand = ControllerCommand_None;
}

//-----------------------------------------------------------------------------
// <Driver::HandleNodeNeighborUpdateRequest>
// Process a request from the Z-Wave PC interface
//-----------------------------------------------------------------------------
void Driver::HandleNodeNeighborUpdateRequest
(
	uint8* _data
)
{
	switch( _data[3] )
	{
		case REQUEST_NEIGHBOR_UPDATE_STARTED:
		{
			Log::Write( "REQUEST_NEIGHBOR_UPDATE_STARTED" );
			if( m_controllerCallback )
			{
				m_controllerCallback( ControllerState_InProgress, m_controllerCallbackContext );
			}
			break;
		}
		case REQUEST_NEIGHBOR_UPDATE_DONE:
		{
			Log::Write( "REQUEST_NEIGHBOR_UPDATE_DONE" );

			// We now request the neighbour information from the
			// controller and store it in our node object.
			RequestNodeNeighbors( m_controllerCommandNode, 0 );
			break;
		}
		case REQUEST_NEIGHBOR_UPDATE_FAILED:
		{
			Log::Write( "REQUEST_NEIGHBOR_UPDATE_FAILED" );
			if( m_controllerCallback )
			{
				m_controllerCallback( ControllerState_Failed, m_controllerCallbackContext );
			}
			m_controllerCommand = ControllerCommand_None;
			break;
		}
		default:
		{
			break;
		}
	}
}

//-----------------------------------------------------------------------------
// <Driver::HandleApplicationUpdateRequest>
// Process a request from the Z-Wave PC interface
//-----------------------------------------------------------------------------
bool Driver::HandleApplicationUpdateRequest
(
	uint8* _data
)
{
	bool messageRemoved = false;

	uint8 nodeId = _data[3];

	switch( _data[2] )
	{
		case UPDATE_STATE_SUC_ID:
		{
			Log::Write( "UPDATE_STATE_SUC_ID from node %d", nodeId );
			break;
		}
		case UPDATE_STATE_DELETE_DONE:
		{
			Log::Write( "** Network change **: Z-Wave node %d was removed", nodeId );

			Notification* notification = new Notification( Notification::Type_NodeRemoved );
			notification->SetHomeAndNodeIds( m_homeId, nodeId );
			QueueNotification( notification ); 

			LockNodes();
			delete m_nodes[nodeId];
			m_nodes[nodeId] = NULL;
			ReleaseNodes();
			break;
		}
		case UPDATE_STATE_NEW_ID_ASSIGNED:
		{
			Log::Write( "** Network change **: ID %d was assigned to a new Z-Wave node", nodeId );
			
			// Request the node protocol info (also removes any existing node and creates a new one)
			InitNode( nodeId );		
			break;
		}
		case UPDATE_STATE_ROUTING_PENDING:
		{
			Log::Write( "UPDATE_STATE_ROUTING_PENDING from node %d", nodeId );
			break;
		}
		case UPDATE_STATE_NODE_INFO_REQ_FAILED:
		{
			Log::Write( "FUNC_ID_ZW_APPLICATION_UPDATE: UPDATE_STATE_NODE_INFO_REQ_FAILED received" );
	
			// Note: Unhelpfully, the nodeId is always zero in this message.  We have to 
			// assume the message came from the last node to which we sent a request.
			if( m_currentMsg )
			{
				Node* node = GetNodeUnsafe( m_currentMsg->GetTargetNodeId() );
				if( node )
				{
					// Retry the query up to three times
					node->QueryStageRetry( Node::QueryStage_NodeInfo, MAX_TRIES );

					// Just in case the failure was due to the node being asleep, we try
					// to move its pending messages to its wakeup queue.  If it is not
					// a sleeping device, this will have no effect.
					if( MoveMessagesToWakeUpQueue( node->GetNodeId() ) )
					{
						messageRemoved = true;
					}
				}
			}
			break;
		}
		case UPDATE_STATE_NODE_INFO_REQ_DONE:
		{
			Log::Write( "UPDATE_STATE_NODE_INFO_REQ_DONE from node %d", nodeId );
			break;
		}
		case UPDATE_STATE_NODE_INFO_RECEIVED:
		{
			Log::Write( "UPDATE_STATE_NODE_INFO_RECEIVED from node %d", nodeId );
			if( Node* node = GetNodeUnsafe( nodeId ) )
			{
				node->UpdateNodeInfo( &_data[8], _data[4] - 3 );
			}
			break;
		}
	}

	if( messageRemoved )
	{
		m_waitingForAck = 0;	
		m_expectedCallbackId = 0;
		m_expectedReply = 0;
		m_expectedCommandClassId = 0;
		m_expectedNodeId = 0;
	}

	return messageRemoved;
}

//-----------------------------------------------------------------------------
// <Driver::CommonAddNodeStatusRequestHandler>
// Handle common AddNode processing for many similar commands
//-----------------------------------------------------------------------------
void Driver::CommonAddNodeStatusRequestHandler
(
	uint8 _funcId,
	uint8* _data
)
{
	switch( _data[3] )
	{
		case ADD_NODE_STATUS_LEARN_READY:
		{
			Log::Write( "ADD_NODE_STATUS_LEARN_READY" );
			m_controllerAdded = false;
			if( m_controllerCallback )
			{
				m_controllerCallback( ControllerState_Waiting, m_controllerCallbackContext );
			}
			break;
		}
		case ADD_NODE_STATUS_NODE_FOUND:
		{
			Log::Write( "ADD_NODE_STATUS_NODE_FOUND" );
			if( m_controllerCallback )
			{
				m_controllerCallback( ControllerState_InProgress, m_controllerCallbackContext );
			}
			break;
		}
		case ADD_NODE_STATUS_ADDING_SLAVE:
		{
			Log::Write( "ADD_NODE_STATUS_ADDING_SLAVE" );			
			Log::Write( "Adding node ID %d", _data[4] );
			m_controllerAdded = false;
			m_controllerCommandNode = _data[4];
			break;
		}
		case ADD_NODE_STATUS_ADDING_CONTROLLER:
		{
			Log::Write( "ADD_NODE_STATUS_ADDING_CONTROLLER");
			Log::Write( "Adding controller ID %d", _data[4] );
			m_controllerAdded = true;
			m_controllerCommandNode = _data[4];
			break;
		}
		case ADD_NODE_STATUS_PROTOCOL_DONE:
		{
			Log::Write( "ADD_NODE_STATUS_PROTOCOL_DONE" );
			if( m_controllerAdded && m_controllerReplication)
			{
				// We added a controller, now is the time to replicate our data to it
				m_controllerReplication->StartReplication( m_controllerCommandNode, _funcId );
			}
			else
			{
				// We added a device.
				// Get the controller out of add mode to avoid accidentally adding other devices.
				Msg* msg = new Msg( "Add Node Mode Stop", 0xff, REQUEST, _funcId, true );
				msg->Append( ADD_NODE_STOP );
				SendMsg( msg, MsgQueue_Command );
			}
			break;
		}
		case ADD_NODE_STATUS_DONE:
		{
			Log::Write( "ADD_NODE_STATUS_DONE" );

			if( m_controllerCommandNode != 0xff )
				InitNode( m_controllerCommandNode );
			if( m_controllerCallback )
			{
				m_controllerCallback( ControllerState_Completed, m_controllerCallbackContext );
			}
			m_controllerCommand = ControllerCommand_None;

			// If the added device was a controller, we should check whether to make it a SUC or SIS
			// TBD...
			break;
		}
		case ADD_NODE_STATUS_FAILED:
		{
			Log::Write( "ADD_NODE_STATUS_FAILED" );
			if( m_controllerCallback )
			{
				m_controllerCallback( ControllerState_Failed, m_controllerCallbackContext );
			}
			m_controllerCommand = ControllerCommand_None;

			// Remove the AddNode command from the queue
			RemoveCurrentMsg();

			// Get the controller out of add mode to avoid accidentally adding other devices.
			Msg* msg = new Msg( "Add Node Stop (Failed)", 0xff, REQUEST, _funcId, true );
			msg->Append( ADD_NODE_STOP_FAILED );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		default:
		{
			break;
		}
	}
}

//-----------------------------------------------------------------------------
//	Polling Z-Wave devices
//-----------------------------------------------------------------------------
	
//-----------------------------------------------------------------------------
// <Driver::EnablePoll>
// Enable polling of a value
//-----------------------------------------------------------------------------
bool Driver::EnablePoll
( 
	ValueID const _valueId
)
{
	// confirm that this node exists
	uint8 nodeId = _valueId.GetNodeId();
    Node* node = GetNode( nodeId );
	if( node != NULL )
	{
		// confirm that this value is in the node's value store
		if( Value* value = node->GetValue( _valueId ) )
		{
			value = value;
			// Add the valueid to the polling list
			m_pollMutex->Lock();

			// See if the node is already in the poll list.
			for( list<ValueID>::iterator it = m_pollList.begin(); it != m_pollList.end(); ++it )
			{
				if( *it == _valueId )
				{
					// It is already in the poll list, so we have nothing to do.
					m_pollMutex->Unlock();
					ReleaseNodes();
					return true;
				}
			}

			// Not in the list, so we add it
			m_pollList.push_back( _valueId );
			m_pollMutex->Unlock();
			ReleaseNodes();
			return true;
		}

		Log::Write( "EnablePoll failed - value not found for node %d", nodeId );
		return false;
	}

	Log::Write( "EnablePoll failed - node %d not found", nodeId );
	return false;
}

//-----------------------------------------------------------------------------
// <Driver::DisablePoll>
// Disable polling of a node
//-----------------------------------------------------------------------------
bool Driver::DisablePoll
( 
	ValueID const _valueId
)
{
	uint8 nodeId = _valueId.GetNodeId();
	Node* node = GetNode( nodeId );
	if( node != NULL)
	{
		m_pollMutex->Lock();

		// See if the value is already in the poll list.
		for( list<ValueID>::iterator it = m_pollList.begin(); it != m_pollList.end(); ++it )
		{
			if( *it == _valueId )
			{
				// Found it
				m_pollList.erase( it );
				m_pollMutex->Unlock();
				ReleaseNodes();
				return true;
			}
		}

		// Not in the list
		m_pollMutex->Unlock();
		ReleaseNodes();
		Log::Write( "DisablePoll failed - value not on list");
		return false;
	}

	Log::Write( "DisablePoll failed - node %d not found", nodeId );
	return false;
}

//-----------------------------------------------------------------------------
// <Driver::isPolled>
// Check polling status of a value
//-----------------------------------------------------------------------------
bool Driver::isPolled
( 
	ValueID const _valueId
)
{
	uint8 nodeId = _valueId.GetNodeId();
	Node* node = GetNode( nodeId );
	if( node != NULL)
	{
		m_pollMutex->Lock();

		// See if the value is already in the poll list.
		for( list<ValueID>::iterator it = m_pollList.begin(); it != m_pollList.end(); ++it )
		{
			if( *it == _valueId )
			{
				// Found it
				m_pollMutex->Unlock();
				ReleaseNodes();
				return true;
			}
		}

		// Not in the list
		m_pollMutex->Unlock();
		ReleaseNodes();
		return false;
	}

	Log::Write( "isPolled failed - node %d not found", nodeId );
	return false;
}

//-----------------------------------------------------------------------------
// <Driver::PollThreadEntryPoint>
// Entry point of the thread for poll Z-Wave devices
//-----------------------------------------------------------------------------
void Driver::PollThreadEntryPoint
( 
	Event* _exitEvent,
	void* _context 
)
{
	Driver* driver = (Driver*)_context;
	if( driver )
	{
		driver->PollThreadProc( _exitEvent );
	}
}

//-----------------------------------------------------------------------------
// <Driver::PollThreadProc>
// Thread for poll Z-Wave devices
//-----------------------------------------------------------------------------
void Driver::PollThreadProc
(
	Event* _exitEvent
)
{
	while( 1 )
	{
		int32 pollInterval = m_pollInterval * 1000;	// Get the time in milliseconds in which we are to poll all the devices

		if( !m_pollList.empty() && m_awakeNodesQueried)
		{
			// We only bother getting the lock if the pollList is not empty
			m_pollMutex->Lock();
			
			if( !m_pollList.empty() )
			{
				// Get the next node to be polled
				ValueID valueId = m_pollList.front();
			
				// Move it to the back of the list
				m_pollList.pop_front();
				m_pollList.push_back( valueId );

				// Calculate the time before the next poll, so that all polls 
				// can take place within the user-specified interval.
				pollInterval /= m_pollList.size();

				// Request the state of the value from the node to which it belongs
				if( Node* node = GetNode( valueId.GetNodeId() ) )
				{
					bool requestState = true;
					if( !node->IsListeningDevice() )
					{
						// The device is not awake all the time.  If it is not awake, we mark it
						// as requiring a poll.  The poll will be done next time the node wakes up.
						if( WakeUp* wakeUp = static_cast<WakeUp*>( node->GetCommandClass( WakeUp::StaticGetCommandClassId() ) ) )
						{
							if( !wakeUp->IsAwake() )
							{
								wakeUp->SetPollRequired();
								requestState = false;
							}
						}
					}

					if( requestState )
					{
						// Request an update of the value
						CommandClass* cc = node->GetCommandClass( valueId.GetCommandClassId() );
						uint8 index = valueId.GetIndex();
						uint8 instance = valueId.GetInstance();
						Log::Write( "Polling node %d: %s index = %d instance = %d (poll queue has %d messages)", node->m_nodeId, cc->GetCommandClassName().c_str(), index, instance, m_msgQueue[MsgQueue_Poll].size() );
						cc->RequestValue( 0, index, instance, MsgQueue_Poll );
					}

					ReleaseNodes();
				}
			}

			m_pollMutex->Unlock();
		}

		// Wait for the interval to expire, while watching for exit events
		if( Wait::Single( _exitEvent, pollInterval ) < 0 )
		{
			// Exit has been called
			return;
		}
	}
}

//-----------------------------------------------------------------------------
//	Retrieving Node information
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// <Driver::InitAllNodes>
// Delete all nodes and fetch new node data from the Z-Wave network
//-----------------------------------------------------------------------------
void Driver::InitAllNodes
(
)
{
	// Delete all the node data
	LockNodes();
	for( int i=0; i<256; ++i )
	{
		if( m_nodes[i] )
		{
			delete m_nodes[i];
			m_nodes[i] = NULL;
		}
	}
	ReleaseNodes();

	// Notify the user that all node and value information has been deleted
	Notification* notification = new Notification( Notification::Type_DriverReset );
	notification->SetHomeAndNodeIds( m_homeId, 0 );
	QueueNotification( notification ); 

	// Fetch new node data from the Z-Wave network
	Msg* msg = new Msg( "InitAllNodes", 0xff, REQUEST, FUNC_ID_SERIAL_API_GET_INIT_DATA, false );
	SendMsg( msg, MsgQueue_Send );
}

//-----------------------------------------------------------------------------
// <Driver::InitNode>
// Queue a node to be interrogated for its setup details
//-----------------------------------------------------------------------------
void Driver::InitNode
( 
	uint8 const _nodeId
)
{
	// Delete any existing node and replace it with a new one
	LockNodes();
	if( m_nodes[_nodeId] )
	{
		// Remove the original node
		Notification* notification = new Notification( Notification::Type_NodeRemoved );
		notification->SetHomeAndNodeIds( m_homeId, _nodeId );
		QueueNotification( notification ); 
		delete m_nodes[_nodeId];
	}

	// Add the new node
	m_nodes[_nodeId] = new Node( m_homeId, _nodeId );
	// Do controller specific node initializations
	if( _nodeId == m_nodeId )
	{
		ManufacturerSpecific::SetProductDetails(m_nodes[_nodeId], m_manufacturerId, m_productType, m_productId);
	}
	ReleaseNodes();

	Notification* notification = new Notification( Notification::Type_NodeAdded );
	notification->SetHomeAndNodeIds( m_homeId, _nodeId );
	QueueNotification( notification ); 

	// Request the node info
	m_nodes[_nodeId]->SetQueryStage( Node::QueryStage_ProtocolInfo );
}

//-----------------------------------------------------------------------------
// <Driver::IsNodeListeningDevice>
// Get whether the node is a listening device that does not go to sleep
//-----------------------------------------------------------------------------
bool Driver::IsNodeListeningDevice
(
	 uint8 const _nodeId
)
{
	bool res = false;
	if( Node* node = GetNode( _nodeId ) )
	{
		res = node->IsListeningDevice();
		ReleaseNodes();
	}

	return res;
}

//-----------------------------------------------------------------------------
// <Driver::IsNodeFrequentListeningDevice>
// Get whether the node is a listening device that does not go to sleep
//-----------------------------------------------------------------------------
bool Driver::IsNodeFrequentListeningDevice
(
	 uint8 const _nodeId
)
{
	bool res = false;
	if( Node* node = GetNode( _nodeId ) )
	{
		res = node->IsFrequentListeningDevice();
		ReleaseNodes();
	}

	return res;
}

//-----------------------------------------------------------------------------
// <Driver::IsNodeBeamingDevice>
// Get whether the node is a beam capable device.
//-----------------------------------------------------------------------------
bool Driver::IsNodeBeamingDevice
(
	 uint8 const _nodeId
)
{
	bool res = false;
	if( Node* node = GetNode( _nodeId ) )
	{
		res = node->IsBeamingDevice();
		ReleaseNodes();
	}

	return res;
}

//-----------------------------------------------------------------------------
// <Driver::IsNodeRoutingDevice>
// Get whether the node is a routing device that passes messages to other nodes
//-----------------------------------------------------------------------------
bool Driver::IsNodeRoutingDevice
(
	 uint8 const _nodeId
)
{
	bool res = false;
	if( Node* node = GetNode( _nodeId ) )
	{
		res = node->IsRoutingDevice();
		ReleaseNodes();
	}

	return res;
}

//-----------------------------------------------------------------------------
// <Driver::IsNodeSecurityDevice>
// Get the security attribute for a node
//-----------------------------------------------------------------------------
bool Driver::IsNodeSecurityDevice
(
	 uint8 const _nodeId
)
{
	bool security = false;
	if( Node* node = GetNode( _nodeId ) )
	{
		security = node->IsSecurityDevice();
		ReleaseNodes();
	}

	return security;
}

//-----------------------------------------------------------------------------
// <Driver::GetNodeMaxBaudRate>
// Get the maximum baud rate of a node's communications
//-----------------------------------------------------------------------------
uint32 Driver::GetNodeMaxBaudRate
(
	 uint8 const _nodeId
)
{
	uint32 baud = 0;
	if( Node* node = GetNode( _nodeId ) )
	{
		baud = node->GetMaxBaudRate();
		ReleaseNodes();
	}

	return baud;
}

//-----------------------------------------------------------------------------
// <Driver::GetNodeVersion>
// Get the version number of a node
//-----------------------------------------------------------------------------
uint8 Driver::GetNodeVersion
(
	 uint8 const _nodeId
)
{
	uint8 version = 0;
	if( Node* node = GetNode( _nodeId ) )
	{
		version = node->GetVersion();
		ReleaseNodes();
	}

	return version;
}

//-----------------------------------------------------------------------------
// <Driver::GetNodeBasic>
// Get the basic type of a node
//-----------------------------------------------------------------------------
uint8 Driver::GetNodeBasic
(
	 uint8 const _nodeId
)
{
	uint8 basic = 0;
	if( Node* node = GetNode( _nodeId ) )
	{
		basic = node->GetBasic();
		ReleaseNodes();
	}

	return basic;
}

//-----------------------------------------------------------------------------
// <Driver::GetNodeGeneric>
// Get the generic type of a node
//-----------------------------------------------------------------------------
uint8 Driver::GetNodeGeneric
(
	 uint8 const _nodeId
)
{
	uint8 genericType = 0;
	if( Node* node = GetNode( _nodeId ) )
	{
		genericType = node->GetGeneric();
		ReleaseNodes();
	}

	return genericType;
}

//-----------------------------------------------------------------------------
// <Driver::GetNodeSpecific>
// Get the specific type of a node
//-----------------------------------------------------------------------------
uint8 Driver::GetNodeSpecific
(
	 uint8 const _nodeId
)
{
	uint8 specific = 0;
	if( Node* node = GetNode( _nodeId ) )
	{
		specific = node->GetSpecific();
		ReleaseNodes();
	}

	return specific;
}

//-----------------------------------------------------------------------------
// <Driver::GetNodeType>
// Get the basic/generic/specific type of the specified node
// Returns a copy of the string rather than a const ref for thread safety
//-----------------------------------------------------------------------------
string Driver::GetNodeType
(
	uint8 const _nodeId
)
{
	if( Node* node = GetNode( _nodeId ) )
	{
		string str = node->GetType();
		ReleaseNodes();
		return str;
	}

	return "Unknown";
}

//-----------------------------------------------------------------------------
// <Driver::GetNodeNeighbors>
// Gets the neighbors for a node
//-----------------------------------------------------------------------------
uint32 Driver::GetNodeNeighbors
(
	uint8 const _nodeId,
	uint8** o_neighbors
)
{
	uint32 numNeighbors = 0;
	if( Node* node = GetNode( _nodeId ) )
	{
		numNeighbors = node->GetNeighbors(o_neighbors );
		ReleaseNodes();
	}

	return numNeighbors;
}

//-----------------------------------------------------------------------------
// <Driver::GetNodeManufacturerName>
// Get the manufacturer name for the node with the specified ID
// Returns a copy of the string rather than a const ref for thread safety
//-----------------------------------------------------------------------------
string Driver::GetNodeManufacturerName
(
	uint8 const _nodeId
)
{
	if( Node* node = GetNode( _nodeId ) )
	{
		string str = node->GetManufacturerName();
		ReleaseNodes();
		return str;
	}

	return "";
}

//-----------------------------------------------------------------------------
// <Driver::GetNodeProductName>
// Get the product name for the node with the specified ID
// Returns a copy of the string rather than a const ref for thread safety
//-----------------------------------------------------------------------------
string Driver::GetNodeProductName
(
	uint8 const _nodeId
)
{
	if( Node* node = GetNode( _nodeId ) )
	{
		string str = node->GetProductName();
		ReleaseNodes();
		return str;
	}

	return "";
}

//-----------------------------------------------------------------------------
// <Driver::GetNodeName>
// Get the user-editable name for the node with the specified ID
// Returns a copy of the string rather than a const ref for thread safety
//-----------------------------------------------------------------------------
string Driver::GetNodeName
(
	uint8 const _nodeId
)
{
	if( Node* node = GetNode( _nodeId ) )
	{
		string str = node->GetNodeName();
		ReleaseNodes();
		return str;
	}

	return "";
}

//-----------------------------------------------------------------------------
// <Driver::GetNodeLocation>
// Get the user-editable string for location of the specified node
// Returns a copy of the string rather than a const ref for thread safety
//-----------------------------------------------------------------------------
string Driver::GetNodeLocation
(
	uint8 const _nodeId
)
{
	if( Node* node = GetNode( _nodeId ) )
	{
		string str = node->GetLocation();
		ReleaseNodes();
		return str;
	}

	return "";
}

//-----------------------------------------------------------------------------
// <Driver::GetNodeManufacturerId>
// Get the manufacturer Id string value with the specified ID
// Returns a copy of the string rather than a const ref for thread safety
//-----------------------------------------------------------------------------
string Driver::GetNodeManufacturerId
(
	uint8 const _nodeId
)
{
	if( Node* node = GetNode( _nodeId ) )
	{
		string str = node->GetManufacturerId();
		ReleaseNodes();
		return str;
	}

	return "";
}

//-----------------------------------------------------------------------------
// <Driver::GetNodeProductType>
// Get the product type string value with the specified ID
// Returns a copy of the string rather than a const ref for thread safety
//-----------------------------------------------------------------------------
string Driver::GetNodeProductType
(
	uint8 const _nodeId
)
{
	if( Node* node = GetNode( _nodeId ) )
	{
		string str = node->GetProductType();
		ReleaseNodes();
		return str;
	}

	return "";
}

//-----------------------------------------------------------------------------
// <Driver::GetNodeProductId>
// Get the product Id string value with the specified ID
// Returns a copy of the string rather than a const ref for thread safety
//-----------------------------------------------------------------------------
string Driver::GetNodeProductId
(
	uint8 const _nodeId
)
{
	if( Node* node = GetNode( _nodeId ) )
	{
		string str = node->GetProductId();
		ReleaseNodes();
		return str;
	}

	return "";
}

//-----------------------------------------------------------------------------
// <Driver::SetNodeManufacturerName>
// Set the manufacturer name for the node with the specified ID
//-----------------------------------------------------------------------------
void Driver::SetNodeManufacturerName
(
	uint8 const _nodeId,
	string const& _manufacturerName
)
{
	if( Node* node = GetNode( _nodeId ) )
	{
		node->SetManufacturerName( _manufacturerName );
		ReleaseNodes();
	}
}

//-----------------------------------------------------------------------------
// <Driver::SetNodeProductName>
// Set the product name string value with the specified ID
//-----------------------------------------------------------------------------
void Driver::SetNodeProductName
(
	uint8 const _nodeId,
	string const& _productName
)
{
	if( Node* node = GetNode( _nodeId ) )
	{
		node->SetProductName( _productName );
		ReleaseNodes();
	}
}

//-----------------------------------------------------------------------------
// <Driver::SetNodeName>
// Set the node name string value with the specified ID
//-----------------------------------------------------------------------------
void Driver::SetNodeName
(
	uint8 const _nodeId,
	string const& _nodeName
)
{
	if( Node* node = GetNode( _nodeId ) )
	{
		node->SetNodeName( _nodeName );
		ReleaseNodes();
	}
}

//-----------------------------------------------------------------------------
// <Driver::SetNodeLocation>
// Set the location string value with the specified ID
//-----------------------------------------------------------------------------
void Driver::SetNodeLocation
(
	uint8 const _nodeId,
	string const& _location
)
{
	if( Node* node = GetNode( _nodeId ) )
	{
		node->SetLocation( _location );
		ReleaseNodes();
	}
}

//-----------------------------------------------------------------------------
// <Driver::SetNodeLevel>
// Helper to set the node level through the basic command class
//-----------------------------------------------------------------------------
void Driver::SetNodeLevel
( 
	uint8 const _nodeId,
	uint8 const _level
)
{
	if( Node* node = GetNode( _nodeId ) )
	{
		node->SetLevel( _level );
		ReleaseNodes();
	}
}

//-----------------------------------------------------------------------------
// <Driver::SetNodeOn>
// Helper to set the node on through the basic command class
//-----------------------------------------------------------------------------
void Driver::SetNodeOn
( 
    uint8 const _nodeId
)
{
    if( Node* node = GetNode( _nodeId ) )
    {
        node->SetNodeOn();
        ReleaseNodes();
    }
}

//-----------------------------------------------------------------------------
// <Driver::SetNodeOff>
// Helper to set the node off through the basic command class
//-----------------------------------------------------------------------------
void Driver::SetNodeOff
( 
    uint8 const _nodeId
)
{
    if( Node* node = GetNode( _nodeId ) )
    {
        node->SetNodeOff();
        ReleaseNodes();
    }
}

//-----------------------------------------------------------------------------
// <Driver::GetValue>
// Get a pointer to a Value object for the specified ValueID
//-----------------------------------------------------------------------------
Value* Driver::GetValue
(
	ValueID const& _id
)
{
	// This method is only called by code that has already locked the node
	if( Node* node = m_nodes[_id.GetNodeId()] )
	{
		return node->GetValue( _id );
	}

	return NULL;
}

//-----------------------------------------------------------------------------
// Controller commands
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// <Driver::ResetController>
// Reset controller and erase all node information
//-----------------------------------------------------------------------------
void Driver::ResetController
(
)
{
	Log::Write( "Reset controller and erase all node information");
	Msg* msg = new Msg( "Reset controller and erase all node information", 0xff, REQUEST, FUNC_ID_ZW_SET_DEFAULT, true );
	SendMsg( msg, MsgQueue_Command );
}

//-----------------------------------------------------------------------------
// <Driver::SoftReset>
// Soft-reset the Z-Wave controller chip
//-----------------------------------------------------------------------------
void Driver::SoftReset
(
)
{
	Log::Write( "Soft-resetting the Z-Wave controller chip");
	Msg* msg = new Msg( "Soft-resetting the Z-Wave controller chip", 0xff, REQUEST, FUNC_ID_SERIAL_API_SOFT_RESET, false, false );
	SendMsg( msg, MsgQueue_Command );
}

//-----------------------------------------------------------------------------
// <Driver::RequestNodeNeighbors>
// Get the neighbour information for a node from the controller
//-----------------------------------------------------------------------------
void Driver::RequestNodeNeighbors
( 
	uint8 const _nodeId,
	uint32 const _requestFlags
)
{
	// Note: This is not the same as RequestNodeNeighbourUpdate.  This method
	// merely requests the controller's current neighbour information and
	// the reply will be copied into the relevant Node object for later use.
	m_controllerCommandNode = _nodeId;
	Log::Write( "Requesting routing info (neighbor list) for Node %d", _nodeId );
	Msg* msg = new Msg( "Get Routing Info", _nodeId, REQUEST, FUNC_ID_ZW_GET_ROUTING_INFO, false );
	msg->Append( _nodeId );
	msg->Append( 1 );		// Exclude bad links
	msg->Append( 1 );		// Exclude non-routing neighbors
	SendMsg( msg, MsgQueue_Command );
}

//-----------------------------------------------------------------------------
// <Driver::BeginControllerCommand>
// Start the controller performing one of its network management functions
//-----------------------------------------------------------------------------
bool Driver::BeginControllerCommand
( 
	ControllerCommand _command,
	pfnControllerCallback_t _callback,
	void* _context,
	bool _highPower,
	uint8 _nodeId
)
{
	if( ControllerCommand_None != m_controllerCommand )
	{
		// Already busy doing something else
		return false;
	}

	m_controllerCallback = _callback;
	m_controllerCallbackContext = _context;
	m_controllerCommand = _command;

	switch( m_controllerCommand )
	{
		case ControllerCommand_AddController:
		{
			Log::Write( "AddController" );
			Msg* msg = new Msg( "AddController", 0xff, REQUEST, FUNC_ID_ZW_ADD_NODE_TO_NETWORK, true );
			msg->Append( _highPower ? ADD_NODE_CONTROLLER | OPTION_HIGH_POWER : ADD_NODE_CONTROLLER );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_AddDevice:
		{
			Log::Write( "AddDevice" );
			Msg* msg = new Msg( "AddDevice", 0xff, REQUEST, FUNC_ID_ZW_ADD_NODE_TO_NETWORK, true );
			msg->Append( _highPower ? ADD_NODE_SLAVE | OPTION_HIGH_POWER : ADD_NODE_SLAVE );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_CreateNewPrimary:
		{
			Log::Write( "CreateNewPrimary" );
			Msg* msg = new Msg( "CreateNewPrimary", 0xff, REQUEST, FUNC_ID_ZW_CREATE_NEW_PRIMARY, true );
			msg->Append( CREATE_PRIMARY_START );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_ReceiveConfiguration:
		{
			Log::Write( "ReceiveConfiguration" );
			Msg* msg = new Msg( "ReceiveConfiguration", 0xff, REQUEST, FUNC_ID_ZW_SET_LEARN_MODE, true );
			msg->Append( 0xff );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_RemoveController:
		{
			Log::Write( "RemoveController" );
			Msg* msg = new Msg( "RemoveController", 0xff, REQUEST, FUNC_ID_ZW_REMOVE_NODE_FROM_NETWORK, true );
			msg->Append( _highPower ? REMOVE_NODE_ANY | OPTION_HIGH_POWER : REMOVE_NODE_ANY );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_RemoveDevice:
		{
			Log::Write( "RemoveDevice" );
			Msg* msg = new Msg( "RemoveDevice", 0xff, REQUEST, FUNC_ID_ZW_REMOVE_NODE_FROM_NETWORK, true );
			msg->Append( _highPower ? REMOVE_NODE_ANY | OPTION_HIGH_POWER : REMOVE_NODE_ANY );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_HasNodeFailed:
		{
			m_controllerCommandNode = _nodeId;
			Log::Write( "Requesting whether node %d has failed", _nodeId );
			Msg* msg = new Msg( "Has Node Failed?", 0xff, REQUEST, FUNC_ID_ZW_IS_FAILED_NODE_ID, false );		
			msg->Append( _nodeId );
			msg->Append( TRANSMIT_OPTION_ACK | TRANSMIT_OPTION_AUTO_ROUTE );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_RemoveFailedNode:
		{
			m_controllerCommandNode = _nodeId;
			Log::Write( "Marking node %d as having failed", _nodeId );
			Msg* msg = new Msg( "Mark Node As Failed", 0xff, REQUEST, FUNC_ID_ZW_REMOVE_FAILED_NODE_ID, true );		
			msg->Append( _nodeId );
			msg->Append( TRANSMIT_OPTION_ACK | TRANSMIT_OPTION_AUTO_ROUTE );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_ReplaceFailedNode:
		{
			m_controllerCommandNode = _nodeId;
			Log::Write( "Replace Failed Node %d", _nodeId );
			Msg* msg = new Msg( "ReplaceFailedNode", 0xff, REQUEST, FUNC_ID_ZW_REPLACE_FAILED_NODE, true );
			msg->Append( _nodeId );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_TransferPrimaryRole:
		{
			Log::Write( "TransferPrimaryRole" );
			Msg* msg = new Msg( "TransferPrimaryRole", 0xff, REQUEST, FUNC_ID_ZW_CONTROLLER_CHANGE, true );
			msg->Append( CONTROLLER_CHANGE_START );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_RequestNetworkUpdate:
		{
			m_controllerCommandNode = _nodeId;
			Log::Write( "RequestNetworkUpdate" );
			Msg* msg = new Msg( "RequestNetworkUpdate", 0xff, REQUEST, FUNC_ID_ZW_REQUEST_NETWORK_UPDATE, true );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_RequestNodeNeighborUpdate:
		{
			m_controllerCommandNode = _nodeId;
			Log::Write( "Requesting Neighbor Update for node %d", _nodeId );
			Msg* msg = new Msg( "Requesting Neighbor Update", _nodeId, REQUEST, FUNC_ID_ZW_REQUEST_NODE_NEIGHBOR_UPDATE, true );
			msg->Append( _nodeId );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_AssignReturnRoute:
		{
			m_controllerCommandNode = _nodeId;
			Log::Write( "Assigning return route from node %d", _nodeId );
			Msg* msg = new Msg( "Assigning return route", _nodeId, REQUEST, FUNC_ID_ZW_ASSIGN_RETURN_ROUTE, true );
			msg->Append( _nodeId );		// from the node
			msg->Append( m_nodeId );	// to the controller
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_DeleteAllReturnRoutes:
		{
			m_controllerCommandNode = _nodeId;
			Log::Write( "Deleting all return routes from node %d", _nodeId );
			Msg* msg = new Msg( "Deleting return routes", _nodeId, REQUEST, FUNC_ID_ZW_DELETE_RETURN_ROUTE, true );
			msg->Append( _nodeId );		// from the node
			SendMsg( msg, MsgQueue_Command );
			break;
		}
        case ControllerCommand_None:
        {
            // To keep gcc quiet
            break;
        }
	}

	return true;
}

//-----------------------------------------------------------------------------
// <Driver::CancelControllerCommand>
// Stop the current controller function
//-----------------------------------------------------------------------------
bool Driver::CancelControllerCommand
( 
)
{
	if( ControllerCommand_None == m_controllerCommand )
	{
		// Controller is not doing anything
		return false;
	}

	switch( m_controllerCommand )
	{
		case ControllerCommand_AddController:
		{
			Log::Write( "CancelAddController" );
			m_controllerCommandNode = 0xff;		// identify the fact that there is no new node to initialize
			Msg* msg = new Msg( "CancelAddController", 0xff, REQUEST, FUNC_ID_ZW_ADD_NODE_TO_NETWORK, true );
			msg->Append( ADD_NODE_STOP );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_AddDevice:
		{
			Log::Write( "CancelAddDevice" );
			m_controllerCommandNode = 0xff;		// identify the fact that there is no new node to initialize
			Msg* msg = new Msg( "CancelAddDevice", 0xff, REQUEST, FUNC_ID_ZW_ADD_NODE_TO_NETWORK, true );
			msg->Append( ADD_NODE_STOP );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_CreateNewPrimary:
		{
			Log::Write( "CancelCreateNewPrimary" );
			Msg* msg = new Msg( "CancelCreateNewPrimary", 0xff, REQUEST, FUNC_ID_ZW_CREATE_NEW_PRIMARY, true );
			msg->Append( CREATE_PRIMARY_STOP );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_ReceiveConfiguration:
		{
			Log::Write( "CancelReceiveConfiguration" );
			Msg* msg = new Msg( "CancelReceiveConfiguration", 0xff, REQUEST, FUNC_ID_ZW_SET_LEARN_MODE, false, false );
			msg->Append( 0 );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_RemoveController:
		{
			Log::Write( "CancelRemoveController" );
			Msg* msg = new Msg( "CancelRemoveController", 0xff, REQUEST, FUNC_ID_ZW_REMOVE_NODE_FROM_NETWORK, true );
			msg->Append( REMOVE_NODE_STOP );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_RemoveDevice:
		{
			Log::Write( "CancelRemoveDevice" );
			Msg* msg = new Msg( "CancelRemoveDevice", 0xff, REQUEST, FUNC_ID_ZW_REMOVE_NODE_FROM_NETWORK, true );
			msg->Append( REMOVE_NODE_STOP );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		case ControllerCommand_RemoveFailedNode:
		case ControllerCommand_HasNodeFailed:
		case ControllerCommand_ReplaceFailedNode:
		{
			// Cannot cancel
			return false;
		}
		case ControllerCommand_TransferPrimaryRole:
		{
			Log::Write( "CancelTransferPrimaryRole" );
			Msg* msg = new Msg( "CancelTransferPrimaryRole", 0xff, REQUEST, FUNC_ID_ZW_CONTROLLER_CHANGE, true );
			msg->Append( CONTROLLER_CHANGE_STOP );
			SendMsg( msg, MsgQueue_Command );
			break;
		}
		
		case ControllerCommand_None:
		case ControllerCommand_RequestNetworkUpdate:
		case ControllerCommand_RequestNodeNeighborUpdate:
		case ControllerCommand_AssignReturnRoute:
		case ControllerCommand_DeleteAllReturnRoutes:
		{
			// To keep gcc quiet
			break;
		}
	}

	m_controllerCommand = ControllerCommand_None;
	return true;
}

//-----------------------------------------------------------------------------
//	SwitchAll
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// <Driver::SwitchAllOn>
// All devices that support the SwitchAll command class will be turned on
//-----------------------------------------------------------------------------
void Driver::SwitchAllOn
(
)
{
	SwitchAll::On( this, 0xff );

	LockNodes();
	for( int i=0; i<256; ++i )
	{
		if( GetNodeUnsafe( i ) )
		{
			if( m_nodes[i]->GetCommandClass( SwitchAll::StaticGetCommandClassId() ) )
			{
				SwitchAll::On( this, (uint8)i );
			}
		}
	}
	ReleaseNodes();
}

//-----------------------------------------------------------------------------
// <Driver::SwitchAllOff>
// All devices that support the SwitchAll command class will be turned off
//-----------------------------------------------------------------------------
void Driver::SwitchAllOff
(
)
{
	SwitchAll::Off( this, 0xff );

	LockNodes();
	for( int i=0; i<256; ++i )
	{
		if( GetNodeUnsafe( i ) )
		{
			if( m_nodes[i]->GetCommandClass( SwitchAll::StaticGetCommandClassId() ) )
			{
				SwitchAll::Off( this, (uint8)i );
			}
		}
	}
	ReleaseNodes();
}

//-----------------------------------------------------------------------------
// <Driver::SetConfigParam>
// Set the value of one of the configuration parameters of a device
//-----------------------------------------------------------------------------
bool Driver::SetConfigParam
(
	uint8 const _nodeId,
	uint8 const _param,
	int32 _value,
	uint8 _size
)
{
	bool res = false;
	if( Node* node = GetNode( _nodeId ) )
	{
		res = node->SetConfigParam( _param, _value, _size );
		ReleaseNodes();
	}

	return res;
}

//-----------------------------------------------------------------------------
// <Driver::RequestConfigParam>
// Request the value of one of the configuration parameters of a device
//-----------------------------------------------------------------------------
void Driver::RequestConfigParam
(
	uint8 const _nodeId,
	uint8 const _param
)
{
	if( Node* node = GetNode( _nodeId ) )
	{
		node->RequestConfigParam( _param );
		ReleaseNodes();
	}
}

//-----------------------------------------------------------------------------
// <Driver::GetNumGroups>
// Gets the number of association groups reported by this node
//-----------------------------------------------------------------------------
uint8 Driver::GetNumGroups
(
	uint8 const _nodeId
)
{
	uint8 numGroups = 0;
	if( Node* node = GetNode( _nodeId ) )
	{
		numGroups = node->GetNumGroups();
		ReleaseNodes();
	}

	return numGroups;
}

//-----------------------------------------------------------------------------
// <Driver::GetAssociations>
// Gets the associations for a group
//-----------------------------------------------------------------------------
uint32 Driver::GetAssociations
( 
	uint8 const _nodeId,
	uint8 const _groupIdx,
	uint8** o_associations
)
{
	uint32 numAssociations = 0;
	if( Node* node = GetNode( _nodeId ) )
	{
		numAssociations = node->GetAssociations( _groupIdx, o_associations );
		ReleaseNodes();
	}

	return numAssociations;
}

//-----------------------------------------------------------------------------
// <Driver::GetMaxAssociations>
// Gets the maximum number of associations for a group
//-----------------------------------------------------------------------------
uint8 Driver::GetMaxAssociations
( 
	uint8 const _nodeId,
	uint8 const _groupIdx
)
{
	uint8 maxAssociations = 0;
	if( Node* node = GetNode( _nodeId ) )
	{
		maxAssociations = node->GetMaxAssociations( _groupIdx );
		ReleaseNodes();
	}

	return maxAssociations;
}

//-----------------------------------------------------------------------------
// <Driver::GetGroupLabel>
// Gets the label for a particular group
//-----------------------------------------------------------------------------
string Driver::GetGroupLabel
( 
	uint8 const _nodeId,
	uint8 const _groupIdx
)
{
	string label = "";
	if( Node* node = GetNode( _nodeId ) )
	{
		label = node->GetGroupLabel( _groupIdx );
		ReleaseNodes();
	}

	return label;
}

//-----------------------------------------------------------------------------
// <Driver::AddAssociation>
// Adds a node to an association group
//-----------------------------------------------------------------------------
void Driver::AddAssociation
(
	uint8 const _nodeId,
	uint8 const _groupIdx,
	uint8 const _targetNodeId
)
{
	if( Node* node = GetNode( _nodeId ) )
	{
		node->AddAssociation( _groupIdx, _targetNodeId );
		ReleaseNodes();
	}
}

//-----------------------------------------------------------------------------
// <Driver::RemoveAssociation>
// Removes a node from an association group
//-----------------------------------------------------------------------------
void Driver::RemoveAssociation
(
	uint8 const _nodeId,
	uint8 const _groupIdx,
	uint8 const _targetNodeId
)
{
	if( Node* node = GetNode( _nodeId ) )
	{
		node->RemoveAssociation( _groupIdx, _targetNodeId );
		ReleaseNodes();
	}
}

//-----------------------------------------------------------------------------
// <Driver::QueueNotification>
// Add a notification to the queue to be sent at a later, safe time.
//-----------------------------------------------------------------------------
void Driver::QueueNotification
(
	Notification* _notification
)
{
	m_notifications.push_back( _notification );
}

//-----------------------------------------------------------------------------
// <Driver::NotifyWatchers>
// Notify any watching objects of a value change
//-----------------------------------------------------------------------------
void Driver::NotifyWatchers
(
)
{
	list<Notification*>::iterator nit = m_notifications.begin();
	while( nit != m_notifications.end() )
	{
		Notification* notification = m_notifications.front();
		m_notifications.pop_front();

		Manager::Get()->NotifyWatchers( notification );

		delete notification;
		nit = m_notifications.begin();
	}
}

//-----------------------------------------------------------------------------
// <Driver::HandleRfPowerLevelSetResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
bool Driver::HandleRfPowerLevelSetResponse
(
	uint8* _data
)
{
	bool res = true;
    // the meaning of this command is currently unclear, and there
    // isn't any returned response data, so just log the function call
	Log::Write("Received reply to FUNC_ID_ZW_R_F_POWER_LEVEL_SET");

	return res; 
}

//-----------------------------------------------------------------------------
// <Driver::HandleSerialApiSetTimeoutsResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
bool Driver::HandleSerialApiSetTimeoutsResponse
(
	uint8* _data
)
{
    // the meaning of this command and its response is currently unclear
    bool res = true;
	Log::Write("Received reply to FUNC_ID_SERIAL_API_SET_TIMEOUTS");
    return res;
}

//-----------------------------------------------------------------------------
// <Driver::HandleMemoryGetByteResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
bool Driver::HandleMemoryGetByteResponse
(
	uint8* _data
)
{
	bool res = true;
    // the meaning of this command and its response is currently unclear
    // it seems to return three bytes of data, so print them out
    Log::Write("Received reply to FUNC_ID_ZW_MEMORY_GET_BYTE, returned data: 0x%02hx 0x%02hx 0x%02hx",
               _data[0], _data[1], _data[2]);

	return res; 
}

//-----------------------------------------------------------------------------
// <Driver::HandleReadMemoryResponse>
// Process a response from the Z-Wave PC interface
//-----------------------------------------------------------------------------
bool Driver::HandleReadMemoryResponse
(
	uint8* _data
)
{
    // the meaning of this command and its response is currently unclear
	bool res = true;
	Log::Write("Received reply to FUNC_ID_MEMORY_GET_BYTE");
	return res; 
}

//-----------------------------------------------------------------------------
// <Driver::GetDriverStatistics>
// Return driver statistics
//-----------------------------------------------------------------------------
void Driver::GetDriverStatistics
(
	DriverData* _data
)
{
	_data->s_SOFCnt = m_SOFCnt;
	_data->s_ACKWaiting = m_ACKWaiting;
	_data->s_readAborts = m_readAborts;
	_data->s_badChecksum = m_badChecksum;
	_data->s_readCnt = m_readCnt;
	_data->s_writeCnt = m_writeCnt;
	_data->s_CANCnt = m_CANCnt;
	_data->s_NAKCnt = m_NAKCnt;
	_data->s_ACKCnt = m_ACKCnt;
	_data->s_OOFCnt = m_OOFCnt;
	_data->s_dropped = m_dropped;
	_data->s_retries = m_retries;
	_data->s_controllerReadCnt = m_controllerReadCnt;
	_data->s_controllerWriteCnt = m_controllerWriteCnt;
}