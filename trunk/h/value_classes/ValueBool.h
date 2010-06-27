//-----------------------------------------------------------------------------
//
//	ValueBool.h
//
//	Represents a boolean value
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

#ifndef _ValueBool_H
#define _ValueBool_H

#include <string>
#include "Defs.h"
#include "Value.h"

class TiXmlElement;

namespace OpenZWave
{
	class Msg;
	class Node;
	class CommandClass;

	class ValueBool: public Value
	{
	public:
		ValueBool( uint32 const _homeId, uint8 const _nodeId, ValueID::ValueGenre const _genre, uint8 const _commandClassId, uint8 const _instance, uint8 const _index, string const& _label, string const& _units, bool const _readOnly, bool const _value );
		ValueBool( uint32 const _homeId, uint8 const _nodeId, uint8 const _commandClassId, TiXmlElement const* _valueElement );
		virtual ~ValueBool(){}

		bool Set( bool const _value );
		void OnValueChanged( bool const _value );

		// From Value
		virtual void WriteXML( TiXmlElement* _valueElement );

		bool GetValue()const{ return m_value; }
		bool GetPending()const{ return m_pending; }

	private:
		bool	m_value;
		bool	m_pending;
	};

} // namespace OpenZWave

#endif



