
// This file is part of the mstp-lib library, available at https://github.com/adigostin/mstp-lib
// Copyright (c) 2011-2019 Adi Gostin, distributed under Apache License v2.0.

#include "stp_base_types.h"
#include "stp_log.h"

bool STP_BRIDGE_ADDRESS::operator== (const STP_BRIDGE_ADDRESS& rhs) const
{
	return memcmp (this->bytes, rhs.bytes, 6) == 0;
}

bool STP_BRIDGE_ADDRESS::operator != (const STP_BRIDGE_ADDRESS& rhs) const
{
	return memcmp (this->bytes, rhs.bytes, 6) != 0;
}

// ============================================================================

const char* GetPortRoleName (STP_PORT_ROLE role)
{
	if (role == STP_PORT_ROLE_MASTER)
		return "Master";
	else if (role == STP_PORT_ROLE_ROOT)
		return "Root";
	else if (role == STP_PORT_ROLE_DESIGNATED)
		return "Designated";
	else if (role == STP_PORT_ROLE_ALTERNATE)
		return "Alternate";
	else if (role == STP_PORT_ROLE_BACKUP)
		return "Backup";
	else if (role == STP_PORT_ROLE_DISABLED)
		return "Disabled";
	else
	{
		assert (false);
		return NULL;
	}
}

// ============================================================================

void PORT_ID::Set (unsigned char priority, unsigned short portNumber)
{
	assert ((priority & 0x0F) == 0);
	assert ((portNumber >= 1) && (portNumber <= 0xFFF));

	_high = priority | (unsigned char) (portNumber >> 8);
	_low = (unsigned char) portNumber;
}

void PORT_ID::Reset ()
{
	_high = 0;
	_low = 0;
}

unsigned char PORT_ID::GetPriority () const
{
	assert (_low != 0); // structure was not initialized; it must have been initialized with Set()
	return _high & 0xF0;
}

void PORT_ID::SetPriority (unsigned char priority)
{
	assert (_low != 0); // structure was not initialized; it must have been initialized with Set()
	assert ((priority & 0x0F) == 0);

	_high = priority | (_high & 0x0F);
}

unsigned short PORT_ID::GetPortNumber () const
{
	assert (_low != 0); // structure was not initialized; it must have been initialized with Set()

	return ((unsigned short) _high & 0x0F) | _low;
}

unsigned short PORT_ID::GetPortIdentifier () const
{
	assert (_low != 0); // structure was not initialized; it must have been initialized with Set()

	unsigned short id = (((unsigned short) _high) << 8) | (unsigned short) _low;
	return id;
}

bool PORT_ID::IsBetterThan (const PORT_ID& rhs) const
{
	assert (_low != 0); // structure was not initialized; it must have been initialized with Set()

	unsigned short lv = (((unsigned short) this->_high) << 8) | (unsigned short) this->_low;
	unsigned short rv = (((unsigned short) rhs._high) << 8) | (unsigned short) rhs._low;

	bool better = (lv < rv);
	return better;
}

bool TIMES::operator== (const TIMES& other) const
{
	return (this->ForwardDelay  == other.ForwardDelay)
		&& (this->HelloTime     == other.HelloTime)
		&& (this->MaxAge        == other.MaxAge)
		&& (this->MessageAge    == other.MessageAge)
		&& (this->remainingHops == other.remainingHops);
}

bool TIMES::operator!= (const TIMES& other) const
{
	return !this->operator==(other);
}
