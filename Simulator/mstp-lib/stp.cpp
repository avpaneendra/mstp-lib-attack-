
// This file is part of the mstp-lib library, available at http://sourceforge.net/projects/mstp-lib/
// Copyright (c) 2011-2014 Adrian Gostin, distributed under the GNU General Public License v3.

#define _CRT_SECURE_NO_WARNINGS

#include "stp.h"
#include "bridge.h"
#include "log.h"
#include "hmac_md5.h"

static void RunStateMachines (STP_BRIDGE* bridge, unsigned int timestamp);
static unsigned int GetInstanceCountForStateMachine (const SM_INFO* smInfo, unsigned int portCount, unsigned int treeCount);
static void RecomputePrioritiesAndPortRoles (STP_BRIDGE* bridge, unsigned int treeIndex, unsigned int timestamp);
static void ComputeMstConfigDigest (STP_BRIDGE* bridge);
//static void GetMstConfigDigest (STP_BRIDGE* bridge, const struct VLAN_TO_MSTID* table, unsigned int tableEntryCount, unsigned char digestOut [16]);

// ============================================================================

STP_BRIDGE* STP_CreateBridge (unsigned int portCount,
						  unsigned int treeCount,
						  const STP_CALLBACKS* callbacks,
						  STP_VERSION protocolVersion,
						  const unsigned char bridgeAddress [6],
						  unsigned int debugLogBufferSize)
{
	// Let's make a few checks on the data types, because we might be compiled with strange
	// compiler options which will turn upside down all our assumptions about structure layouts.
	// These really should use static_assert, but I'm not sure all compilers support static_assert.
	// If you get one of these asserts, you should reset your compiler options to their defaults,
	// at least those options related to structure layouts, at least for the files belonging to the STP library.
	assert (sizeof (unsigned int) == 4);
	assert (sizeof (INV_UINT2) == 2);
	assert (sizeof (INV_UINT4) == 4);
	assert (sizeof (BRIDGE_ADDRESS) == 6);
	assert (sizeof (BRIDGE_ID) == 8);
	assert (sizeof (PORT_ID) == 2);
	assert (sizeof (PRIORITY_VECTOR) == 34);
	assert (sizeof (MSTP_BPDU) == 102);

	assert (debugLogBufferSize >= 2); // one byte for the data, one for the null terminator of the string passed to the callback

	STP_BRIDGE* bridge = (STP_BRIDGE*) callbacks->allocAndZeroMemory (sizeof (STP_BRIDGE));
	assert (bridge != NULL);

	if (protocolVersion == STP_VERSION_LEGACY_STP)
	{
		assert (treeCount == 1);
	}
	else if (protocolVersion == STP_VERSION_RSTP)
	{
		assert (treeCount == 1);
	}
	else if (protocolVersion == STP_VERSION_MSTP)
	{
		// Upper limit for number of MSTIs is defined in 802.1Q-2011, page 342, top paragraph:
		//		"No more than 64 MSTI Configuration Messages shall be encoded in an MST
		//		BPDU, and no more than 64 MSTIs shall be supported by an MST Bridge."
		// So CIST + 64 MSTIs = 65 trees.

		assert ((treeCount >= 1) && (treeCount <= 65));
	}
	else
		assert (false);

	// As specified in 12.3.i) in 802.1Q-2011, valid port numbers are 1..4095, so our valid port indexes will be 0..4094.
	// This means a maximum of 4095 ports.
	assert ((portCount >= 1) && (portCount < 4096));

	// See "13.6.2 Force Protocol Version" on page 332
	bridge->ForceProtocolVersion = protocolVersion;

	bridge->smInterface = &smInterface_802_1Q_2011;
	bridge->callbacks = *callbacks;
	bridge->portCount = portCount;
	bridge->treeCount = treeCount;

	bridge->logBuffer = (char*) callbacks->allocAndZeroMemory (debugLogBufferSize);
	assert (bridge->logBuffer != NULL);
	bridge->logBufferMaxSize = debugLogBufferSize;
	bridge->logBufferUsedSize = 0;
	bridge->logCurrentPort = -1;
	bridge->logCurrentTree = -1;

	// ------------------------------------------------------------------------
	// alloc space for state array
	unsigned int stateMachineInstanceCount = 0;
	for (unsigned int i = 0; i < bridge->smInterface->smInfoCount; i++)
		stateMachineInstanceCount += GetInstanceCountForStateMachine (&bridge->smInterface->smInfo [i], portCount, treeCount);

	stateMachineInstanceCount += GetInstanceCountForStateMachine (bridge->smInterface->transmitSmInfo, portCount, treeCount);

	bridge->states = (SM_STATE*) callbacks->allocAndZeroMemory (stateMachineInstanceCount * sizeof (SM_STATE));
	assert (bridge->states != NULL);

	// ------------------------------------------------------------------------

	bridge->trees = (BRIDGE_TREE**) callbacks->allocAndZeroMemory (treeCount * sizeof (BRIDGE_TREE*));
	assert (bridge->trees != NULL);

	bridge->ports = (PORT**) callbacks->allocAndZeroMemory (portCount * sizeof (PORT*));
	assert (bridge->ports != NULL);

	// per-bridge CIST vars
	bridge->trees [CIST_INDEX] = (BRIDGE_TREE*) callbacks->allocAndZeroMemory (sizeof (BRIDGE_TREE));
	assert (bridge->trees [CIST_INDEX] != NULL);
	bridge->trees [CIST_INDEX]->SetBridgeIdentifier (0x8000, CIST_INDEX, bridgeAddress);
	// 13.24.3 in 802.1Q-2011
	bridge->trees [CIST_INDEX]->BridgeTimes.HelloTime		= STP_BRIDGE::BridgeHelloTime;
	bridge->trees [CIST_INDEX]->BridgeTimes.remainingHops	= bridge->MaxHops;
	bridge->trees [CIST_INDEX]->BridgeTimes.ForwardDelay	= bridge->BridgeForwardDelay;
	bridge->trees [CIST_INDEX]->BridgeTimes.MaxAge			= bridge->BridgeMaxAge;
	bridge->trees [CIST_INDEX]->BridgeTimes.MessageAge		= 0;

	// per-bridge MSTI vars
	for (unsigned int treeIndex = 1; treeIndex < treeCount; treeIndex++)
	{
		bridge->trees [treeIndex] = (BRIDGE_TREE*) callbacks->allocAndZeroMemory (sizeof (BRIDGE_TREE));
		assert (bridge->trees [treeIndex] != NULL);
		bridge->trees [treeIndex]->SetBridgeIdentifier (0x8000, treeIndex, bridgeAddress);
		bridge->trees [treeIndex]->BridgeTimes.remainingHops = bridge->MaxHops;
	}

	// per-port vars
	for (unsigned int portIndex = 0; portIndex < bridge->portCount; portIndex++)
	{
		bridge->ports [portIndex] = (PORT*) callbacks->allocAndZeroMemory (sizeof (PORT));
		assert (bridge->ports [portIndex] != NULL);

		PORT* port = bridge->ports [portIndex];

		port->trees = (PORT_TREE**) callbacks->allocAndZeroMemory (treeCount * sizeof (PORT_TREE*));
		assert (port->trees != NULL);

		// per-port CIST and MSTI vars
		for (unsigned int treeIndex = 0; treeIndex < treeCount; treeIndex++)
		{
			port->trees [treeIndex] = (PORT_TREE*) callbacks->allocAndZeroMemory (sizeof (PORT_TREE));
			assert (port->trees [treeIndex] != NULL);
			port->trees [treeIndex]->portId.Set (0x80, (unsigned short) portIndex + 1);
			port->trees [treeIndex]->portTimes.HelloTime = STP_BRIDGE::BridgeHelloTime;
			port->trees [treeIndex]->InternalPortPathCost = 200000;
		}

		port->AutoEdge = 1;
		port->enableBPDUrx = true;
		port->enableBPDUtx = true;
		port->ExternalPortPathCost = 200000;
	}

	bridge->receivedBpduContent = NULL; // see comment at declaration of receivedBpduContent

	// These were already zeroed by the allocation routine.
	//bridge->MstConfigId.ConfigurationIdentifierFormatSelector = 0;
	//bridge->MstConfigId.RevisionLevel = 0;

	// Let's set a default name for the MST Config. Cisco uses lowercase letters here, so let's do the same
	sprintf (bridge->MstConfigId.ConfigurationName, "%02x:%02x:%02x:%02x:%02x:%02x",
		bridgeAddress [0], bridgeAddress [1], bridgeAddress [2],
		bridgeAddress [3], bridgeAddress [4], bridgeAddress [5]);

	// The config table is all zeroes now, so all VIDs map to the CIST, no VID mapped to any MSTI.
	ComputeMstConfigDigest (bridge);

	return bridge;
}

// ============================================================================

void STP_DestroyBridge (STP_BRIDGE* bridge)
{
	for (unsigned int portIndex = 0; portIndex < bridge->portCount; portIndex++)
	{
		for (unsigned int treeIndex = 0; treeIndex < bridge->treeCount; treeIndex++)
			bridge->callbacks.freeMemory (bridge->ports [portIndex]->trees [treeIndex]);

		bridge->callbacks.freeMemory (bridge->ports [portIndex]->trees);

		bridge->callbacks.freeMemory (bridge->ports [portIndex]);
	}

	for (unsigned int treeIndex = 0; treeIndex < bridge->treeCount; treeIndex++)
		bridge->callbacks.freeMemory (bridge->trees [treeIndex]);

	bridge->callbacks.freeMemory (bridge->ports);
	bridge->callbacks.freeMemory (bridge->trees);
	bridge->callbacks.freeMemory (bridge->states);
	bridge->callbacks.freeMemory (bridge->logBuffer);

	bridge->callbacks.freeMemory (bridge);
}

// ============================================================================

void STP_StartBridge (STP_BRIDGE* bridge, unsigned int timestamp)
{
	LOG (bridge, -1, -1, "{T}: Starting the bridge...\r\n", timestamp);

	assert (bridge->started == false);

	bridge->started = true;

	bridge->BEGIN = true;
	RunStateMachines (bridge, timestamp);
	bridge->BEGIN = false;
	RunStateMachines (bridge, timestamp);

	LOG (bridge, -1, -1, "Bridge started.\r\n");
	LOG (bridge, -1, -1, "------------------------------------\r\n");
	FLUSH_LOG (bridge);
}

// ============================================================================

void STP_StopBridge (STP_BRIDGE* bridge, unsigned int timestamp)
{
	bridge->started = false;

	LOG (bridge, -1, -1, "{T}: Bridge stopped.\r\n", timestamp);
	LOG (bridge, -1, -1, "------------------------------------\r\n");
	FLUSH_LOG (bridge);
}

// ============================================================================

void STP_SetBridgeAddress (STP_BRIDGE* bridge, const unsigned char* address, unsigned int timestamp)
{
	LOG (bridge, -1, -1, "{T}: Setting bridge MAC address to {BA}...\r\n", timestamp, address);

	for (unsigned int treeIndex = 0; treeIndex < bridge->treeCount; treeIndex++)
	{
		// change the MAC address without changing the priority
		BRIDGE_ID bid = bridge->trees [treeIndex]->GetBridgeIdentifier ();
		bid.SetAddress (address);
		bridge->trees [treeIndex]->SetBridgeIdentifier (bid);
	}

	if (bridge->ForceProtocolVersion < STP_VERSION_MSTP)
	{
		if (bridge->started)
		{
			// STP or RSTP mode. I think there's no need to assert BEGIN, only to recompute priorities.
			RecomputePrioritiesAndPortRoles (bridge, CIST_INDEX, timestamp);
		}
	}
	else
	{
		if (bridge->started)
		{
			// BEGIN used to be asserted when the MST Config Name was generated from the bridge address.
			// Now that we don't generate a default name anymore, I don't know if it's still needed, but I'll leave it for now.
			bridge->BEGIN = true;
			RunStateMachines (bridge, timestamp);
			bridge->BEGIN = false;
			RunStateMachines (bridge, timestamp);
		}
	}

	LOG (bridge, -1, -1, "------------------------------------\r\n");
	FLUSH_LOG (bridge);
}

void STP_GetBridgeAddress (STP_BRIDGE* bridge, unsigned char* addressOut6Bytes)
{
	const unsigned char* address = (const unsigned char*) bridge->trees [CIST_INDEX]->GetBridgeIdentifier ().GetAddress ();
	memcpy (addressOut6Bytes, address, 6);
}

// ============================================================================

void STP_OnPortEnabled (STP_BRIDGE* bridge, unsigned int portIndex, unsigned int speedMegabitsPerSecond, bool detectedPointToPointMAC, unsigned int timestamp)
{
	LOG (bridge, -1, -1, "{T}: Port {D} good\r\n", timestamp, 1 + portIndex);

	PORT* port = bridge->ports [portIndex];

	// Check that this function is not called twice in a row for the same port.
	assert (port->portEnabled == false);

	port->portEnabled = true;

	port->detectedPointToPointMAC = detectedPointToPointMAC;

	// If STP_OnPortEnabled is called for the first time after software startup,
	// and if STP_SetPortAdminP2P was not yet called or called with AUTO,
	// then operPointToPointMAC was never computed.
	// So let's force a computation here to account for this case.
	if (port->adminPointToPointMAC == STP_ADMIN_P2P_AUTO)
		port->operPointToPointMAC = detectedPointToPointMAC;

	if (bridge->started)
		RunStateMachines (bridge, timestamp);

	LOG (bridge, -1, -1, "------------------------------------\r\n");
	FLUSH_LOG (bridge);
}

// ============================================================================

void STP_OnPortDisabled (STP_BRIDGE* bridge, unsigned int portIndex, unsigned int timestamp)
{
	LOG (bridge, -1, -1, "{T}: Port {D} down\r\n", timestamp, 1 + portIndex);

	// We allow disabling an already disabled port.
	if (bridge->ports [portIndex]->portEnabled)
	{
		bridge->ports [portIndex]->portEnabled = false;

		if (bridge->started)
			RunStateMachines (bridge, timestamp);
	}

	LOG (bridge, -1, -1, "------------------------------------\r\n");
	FLUSH_LOG (bridge);
}

// ============================================================================

void STP_OnOneSecondTick (STP_BRIDGE* bridge, unsigned int timestamp)
{
	if (bridge->started)
	{
		LOG (bridge, -1, -1, "{T}: One second:\r\n", timestamp);

		// Not from the standard. See long comment in 802_1Q_2011_procedures.cpp, just above CallTcCallback().
		if (bridge->tcIgnore > 0)
			bridge->tcIgnore--;

		for (unsigned int givenPort = 0; givenPort < bridge->portCount; givenPort++)
			bridge->ports [givenPort]->tick = true;

		RunStateMachines (bridge, timestamp);

		LOG (bridge, -1, -1, "------------------------------------\r\n");
		FLUSH_LOG (bridge);
	}
}

// ============================================================================

void STP_OnBpduReceived (STP_BRIDGE* bridge, unsigned int portIndex, const unsigned char* bpdu, unsigned int bpduSize, unsigned int timestamp)
{
	if (bridge->started)
	{
		if (bridge->ports [portIndex]->portEnabled == false)
		{
			LOG (bridge, -1, -1, "{T}: WARNING: BPDU received on disabled port {D}. The STP library is discarding it.\r\n", timestamp, 1 + portIndex);
		}
		else
		{
			LOG (bridge, -1, -1, "{T}: BPDU received on Port {D}:\r\n", timestamp, 1 + portIndex);

			enum VALIDATED_BPDU_TYPE type = STP_GetValidatedBpduType (bpdu, bpduSize);
			bool passToStateMachines;
			switch (type)
			{
				case VALIDATED_BPDU_TYPE_STP_CONFIG:
					LOG (bridge, portIndex, -1, "Config BPDU:\r\n");
					LOG_INDENT (bridge);
					DumpConfigBpdu (bridge, portIndex, -1, (const MSTP_BPDU*) bpdu);
					LOG_UNINDENT (bridge);
					passToStateMachines = true;
					break;

				case VALIDATED_BPDU_TYPE_RST:
					LOG (bridge, portIndex, -1, "RSTP BPDU:\r\n");
					LOG_INDENT (bridge);
					DumpRstpBpdu (bridge, portIndex, -1, (const MSTP_BPDU*) bpdu);
					LOG_UNINDENT (bridge);
					passToStateMachines = true;
					break;

				case VALIDATED_BPDU_TYPE_MST:
					LOG (bridge, portIndex, -1, "MSTP BPDU:\r\n");
					LOG_INDENT (bridge);
					DumpMstpBpdu (bridge, portIndex, -1, (const MSTP_BPDU*) bpdu);
					LOG_UNINDENT (bridge);
					passToStateMachines = true;
					break;

				case VALIDATED_BPDU_TYPE_STP_TCN:
					LOG (bridge, portIndex, -1, "TCN BPDU.\r\n");
					passToStateMachines = true;
					break;

				default:
					LOG (bridge, portIndex, -1, "Invalid BPDU received. Discarding it.\r\n");
					passToStateMachines = false;
			}

			if (passToStateMachines)
			{
				assert (bridge->receivedBpduContent == NULL);
				assert (bridge->receivedBpduType == VALIDATED_BPDU_TYPE_UNKNOWN);
				assert (bridge->ports [portIndex]->rcvdBpdu == false);

				bridge->receivedBpduContent = (MSTP_BPDU*) bpdu;
				bridge->receivedBpduType = type;
				bridge->ports [portIndex]->rcvdBpdu = true;

				RunStateMachines (bridge, timestamp);

				bridge->receivedBpduContent = NULL; // to cause an exception on access
				bridge->receivedBpduType = VALIDATED_BPDU_TYPE_UNKNOWN; // to cause asserts on access

				// Check that the state machines did process the BPDU.
				assert (bridge->ports [portIndex]->rcvdBpdu == false);
			}
		}

		LOG (bridge, -1, -1, "------------------------------------\r\n");
		FLUSH_LOG (bridge);
	}
}

// ============================================================================

bool STP_IsBridgeStarted (STP_BRIDGE* bridge)
{
	return bridge->started;
}

// ============================================================================

void STP_EnableLogging (STP_BRIDGE* bridge, bool enable)
{
	bridge->loggingEnabled = enable;
}

// ============================================================================

bool STP_IsLoggingEnabled (STP_BRIDGE* bridge)
{
	return bridge->loggingEnabled;
}

// ============================================================================

static bool RunStateMachineInstance (STP_BRIDGE* bridge, const SM_INFO* smInfo, int givenPort, int givenTree, SM_STATE* statePtr, unsigned int timestamp)
{
	volatile bool changed = false;

rep:
	SM_STATE newState = smInfo->checkConditions (bridge, givenPort, givenTree, *statePtr);
	if (newState != 0)
	{
		if (givenPort == -1)
			LOG (bridge, givenPort, givenTree, "Bridge: ");
		else
			LOG (bridge, givenPort, givenTree, "Port {D}: ", 1 + givenPort);

		if (bridge->ForceProtocolVersion >= STP_VERSION_MSTP)
		{
			if (givenTree == CIST_INDEX)
				LOG (bridge, givenPort, givenTree, "CIST: ");
			else if (givenTree > 0)
				LOG (bridge, givenPort, givenTree, "MST{D}: ", givenTree);
		}

		//const char* currentStateName = smInfo->getStateName (*statePtr);
		const char* newStateName = smInfo->getStateName (newState);

		//LOG (bridge, givenPort, givenTree, "{S}: {S} -> {S}\r\n", smInfo->smName, currentStateName, newStateName);
		LOG (bridge, givenPort, givenTree, "{S}: -> {S}\r\n", smInfo->smName, newStateName);

		smInfo->initState (bridge, givenPort, givenTree, newState, timestamp);

		*statePtr = newState;
		changed = true;
		goto rep;
	}

	return changed;
}

// ============================================================================

static bool RunStateMachineInstances (STP_BRIDGE* bridge, const SM_INFO* smInfo, SM_STATE** statePtr, unsigned int timestamp)
{
	volatile bool changed = false;

	switch (smInfo->instanceType)
	{
		case SM_INFO::PER_BRIDGE:
			changed |= RunStateMachineInstance (bridge, smInfo, -1, -1, *statePtr, timestamp);
			(*statePtr)++;
			break;

		case SM_INFO::PER_BRIDGE_PER_TREE:
			for (unsigned int treeIndex = 0; treeIndex < bridge->treeCount; treeIndex++)
			{
				changed |= RunStateMachineInstance (bridge, smInfo, -1, treeIndex, *statePtr, timestamp);
				(*statePtr)++;
			}
			break;

		case SM_INFO::PER_PORT:
			for (unsigned int portIndex = 0; portIndex < bridge->portCount; portIndex++)
			{
				changed |= RunStateMachineInstance (bridge, smInfo, portIndex, -1, *statePtr, timestamp);
				(*statePtr)++;
			}
			break;

		case SM_INFO::PER_PORT_PER_TREE:
			for (unsigned int treeIndex = 0; treeIndex < bridge->treeCount; treeIndex++)
			{
				for (unsigned int portIndex = 0; portIndex < bridge->portCount; portIndex++)
				{
					changed |= RunStateMachineInstance (bridge, smInfo, portIndex, treeIndex, *statePtr, timestamp);
					(*statePtr)++;
				}
			}
			break;
	}

	return changed;
}

// ============================================================================

static void RunStateMachines (STP_BRIDGE* bridge, unsigned int timestamp)
{
	volatile bool changed;

	do
	{
		SM_STATE* statePtr = bridge->states;

		changed = false;

		for (unsigned int i = 0; i < bridge->smInterface->smInfoCount; i++)
			changed |= RunStateMachineInstances (bridge, &bridge->smInterface->smInfo [i], &statePtr, timestamp);

		// We execute the PortTransmit state machine only after all other state machines have finished executing,
		// so as to avoid transmitting BPDUs containing results from intermediary calculations.
		// I remember reading this in the standard somewhere.
		if (changed == false)
			changed |= RunStateMachineInstances (bridge, bridge->smInterface->transmitSmInfo, &statePtr, timestamp);

	} while (changed);
}

// ============================================================================

static unsigned int GetInstanceCountForStateMachine (const SM_INFO* smInfo, unsigned int portCount, unsigned int treeCount)
{
	switch (smInfo->instanceType)
	{
		case SM_INFO::PER_BRIDGE:			return 1;
		case SM_INFO::PER_PORT:				return portCount;
		case SM_INFO::PER_BRIDGE_PER_TREE:	return treeCount;
		case SM_INFO::PER_PORT_PER_TREE:	return treeCount * portCount;
		default:
			assert (false);
			return 0;
	}
}

// ============================================================================

void STP_SetPortAdminEdge (STP_BRIDGE* bridge, unsigned int portIndex, bool newAdminEdge, unsigned int timestamp)
{
	bridge->ports [portIndex]->AdminEdge = newAdminEdge;
}

bool STP_GetPortAdminEdge (STP_BRIDGE* bridge, unsigned int portIndex)
{
	return bridge->ports [portIndex]->AdminEdge;
}

// ============================================================================

void STP_SetPortAutoEdge (STP_BRIDGE* bridge, unsigned int portIndex, bool newAutoEdge, unsigned int timestamp)
{
	bridge->ports [portIndex]->AutoEdge = newAutoEdge;
}

bool STP_GetPortAutoEdge (STP_BRIDGE* bridge, unsigned int portIndex)
{
	return bridge->ports [portIndex]->AutoEdge;
}

// ============================================================================

void STP_SetPortAdminPointToPointMAC (STP_BRIDGE* bridge, unsigned int portIndex, STP_ADMIN_P2P adminPointToPointMAC, unsigned int timestamp)
{
	const char* p2pString = STP_GetAdminP2PString (adminPointToPointMAC);
	LOG (bridge, portIndex, -1, "{T}: Setting adminPointToPointMAC = {S} on port {D}...\r\n", timestamp, p2pString, 1 + portIndex);

	PORT* port = bridge->ports [portIndex];

	if (port->adminPointToPointMAC != adminPointToPointMAC)
	{
		port->adminPointToPointMAC = adminPointToPointMAC;

		if (adminPointToPointMAC == STP_ADMIN_P2P_FORCE_TRUE)
		{
			port->operPointToPointMAC = true;
		}
		else if (adminPointToPointMAC == STP_ADMIN_P2P_FORCE_FALSE)
		{
			port->operPointToPointMAC = false;
		}
		else if (adminPointToPointMAC == STP_ADMIN_P2P_AUTO)
		{
			port->operPointToPointMAC = port->detectedPointToPointMAC;
		}
		else
		{
			assert (false);
		}

		// operPointToPointMAC has changed, and there's logic in the STP state machines that depends on it,
		// but reruning the state machines here seems to me like overkill; they run anyway every second.
	}

	LOG (bridge, -1, -1, "------------------------------------\r\n");
	FLUSH_LOG (bridge);
}

STP_ADMIN_P2P STP_GetPortAdminPointToPointMAC (STP_BRIDGE* bridge, unsigned int portIndex)
{
	return bridge->ports [portIndex]->adminPointToPointMAC;
}

// ============================================================================

static void RecomputePrioritiesAndPortRoles (STP_BRIDGE* bridge, unsigned int treeIndex, unsigned int timestamp)
{
	// From page 357 of 802.1Q-2011:
	// BridgeIdentifier, BridgePriority, and BridgeTimes are not modified by the operation of the spanning tree
	// protocols but are treated as constants by the state machines. If they are modified by management, spanning
	// tree priority vectors and Port Role assignments for shall be recomputed, as specified by the operation of the
	// Port Role Selection state machine (13.34) by clearing selected (13.25) and setting reselect (13.25) for all
	// Bridge Ports for the relevant MSTI and for all trees if the CIST parameter is changed.

	if (treeIndex == CIST_INDEX)
	{
		// Recompute all trees.
		// Note that callers of this function expect recomputation for all trees when CIST_INDEX is passed, so don't change this functionality.
		for (treeIndex = 0; treeIndex < bridge->treeCount; treeIndex++)
		{
			for (unsigned int portIndex = 0; portIndex < bridge->portCount; portIndex++)
			{
				PORT_TREE* portTree = bridge->ports [portIndex]->trees [treeIndex];
				portTree->selected = false;
				portTree->reselect = true;
			}
		}
	}
	else
	{
		// recompute specified MSTI
		for (unsigned int portIndex = 0; portIndex < bridge->portCount; portIndex++)
		{
			PORT_TREE* portTree = bridge->ports [portIndex]->trees [treeIndex];
			portTree->selected = false;
			portTree->reselect = true;
		}
	}

	RunStateMachines (bridge, timestamp);
}

// Problem when setting a worse bridge priority (numerically higher)
// on the root bridge, and that bridge remains root even with the new priority:
//
// BPDUs with the old priority could still be propagating throughout the network, until they are discarded
// due to MaxAge / remainingHops. These BPDUs will mess up all priority calculations while propagating,
// because they have the same bridge address, so they will be Superior to the BPDUs newly generated by the same root.
//
// This increases the convergence time by up to HelloTime seconds, and it seems to be a problem of the protocol itself.
// If compounded with delays introduced either by other problems of the protocol, or by problems in the rest of the firmware,
// it might leads to the formation of loops.
//
// I don't think this could be resolved given the current BPDU format. In a future version of the protocol it could be resolved,
// for example, by encoding a timestamp in BPDUs, and using this timestamp to determine whether a received BPDU is Superior.

void STP_SetBridgePriority (STP_BRIDGE* bridge, unsigned int treeIndex, unsigned short bridgePriority, unsigned int timestamp)
{
	// See table 13-3 on page 348 of 802.1Q-2011

	assert ((bridgePriority & 0x0FFF) == 0);

	assert (treeIndex < bridge->treeCount);

	LOG (bridge, -1, -1, "{T}: Setting bridge priority: tree {TN} prio = {D}...\r\n",
		 timestamp,
		 treeIndex,
		 bridgePriority);

	BRIDGE_ID bid = bridge->trees [treeIndex]->GetBridgeIdentifier ();
	bid.SetPriority (bridgePriority, treeIndex);
	bridge->trees [treeIndex]->SetBridgeIdentifier (bid);

	if (bridge->started)
		RecomputePrioritiesAndPortRoles (bridge, treeIndex, timestamp);

	LOG (bridge, -1, -1, "------------------------------------\r\n");
	FLUSH_LOG (bridge);
}

unsigned short STP_GetBridgePriority (STP_BRIDGE* bridge, unsigned int treeIndex)
{
	assert (treeIndex < bridge->treeCount);

	return bridge->trees [treeIndex]->GetBridgeIdentifier ().GetPriority () & 0xF000;
}

// ============================================================================

void STP_SetPortPriority (STP_BRIDGE* bridge, unsigned int portIndex, unsigned int treeIndex, unsigned char portPriority, unsigned int timestamp)
{
	// See table 13-3 on page 348 of 802.1Q-2011
	// See 13.25.32 in 802.1Q-2011

	assert ((portPriority % 16) == 0);

	assert (portIndex < bridge->portCount);
	assert (treeIndex < bridge->treeCount);

	LOG (bridge, -1, -1, "{T}: Setting port priority: port {D} tree {TN} prio = {D}...\r\n",
		 timestamp,
		 1 + portIndex,
		 treeIndex,
		 portPriority);

	bridge->ports [portIndex]->trees [treeIndex]->portId.SetPriority (portPriority);

	// It would make sense that stuff is recomputed also when the port priority in the portId variable
	// is changed (as it is recomputed for the bridge priority), but either the spec does not mention this, or I'm not seeing it.
	// Anyway, information about the new port priority can only be propagated by such a recomputation, so let's do that.
	if (bridge->started)
		RecomputePrioritiesAndPortRoles (bridge, treeIndex, timestamp);

	LOG (bridge, -1, -1, "------------------------------------\r\n");
	FLUSH_LOG (bridge);
}

unsigned char STP_GetPortPriority (STP_BRIDGE* bridge, unsigned int portIndex, unsigned int treeIndex)
{
	// See 13.25.32 in 802.1Q-2011

	assert (portIndex < bridge->portCount);
	assert (treeIndex < bridge->treeCount);

	unsigned char priority = bridge->ports [portIndex]->trees [treeIndex]->portId.GetPriority ();
	return priority;
}

unsigned short STP_GetPortIdentifier (STP_BRIDGE* bridge, unsigned int portIndex, unsigned int treeIndex)
{
	assert (portIndex < bridge->portCount);
	assert (treeIndex < bridge->treeCount);

	unsigned short id = bridge->ports [portIndex]->trees [treeIndex]->portId.GetPortIdentifier ();
	return id;
}

// ============================================================================

void STP_GetMstConfigName (STP_BRIDGE* bridge, char nameOut [33])
{
	assert (bridge->ForceProtocolVersion >= STP_VERSION_MSTP);

	memcpy (nameOut, bridge->MstConfigId.ConfigurationName, 32);
	nameOut [32] = 0;
}

void STP_SetMstConfigName (STP_BRIDGE* bridge, const char* name, unsigned int timestamp)
{
	assert (bridge->ForceProtocolVersion >= STP_VERSION_MSTP);
	assert (strlen (name) <= 32);

	LOG (bridge, -1, -1, "{T}: Setting MST Config Name to \"{S}\"...\r\n", timestamp, name);

	memset (bridge->MstConfigId.ConfigurationName, 0, 32);
	memcpy (bridge->MstConfigId.ConfigurationName, name, strlen (name));

	if (bridge->started)
	{
		bridge->BEGIN = true;
		RunStateMachines (bridge, timestamp);
		bridge->BEGIN = false;
		RunStateMachines (bridge, timestamp);
	}

	LOG (bridge, -1, -1, "------------------------------------\r\n");
	FLUSH_LOG (bridge);
}

// ============================================================================

void STP_SetMstConfigRevisionLevel (STP_BRIDGE* bridge, unsigned short revisionLevel, unsigned int timestamp)
{
	assert (bridge->ForceProtocolVersion >= STP_VERSION_MSTP);

	if (bridge->MstConfigId.RevisionLevel != revisionLevel)
	{
		LOG (bridge, -1, -1, "{T}: Setting MST Config Revision Level to {D}...\r\n", timestamp, (int) revisionLevel);

		bridge->MstConfigId.RevisionLevel = revisionLevel;

		if (bridge->started)
		{
			bridge->BEGIN = true;
			RunStateMachines (bridge, timestamp);
			bridge->BEGIN = false;
			RunStateMachines (bridge, timestamp);
		}

		LOG (bridge, -1, -1, "------------------------------------\r\n");
		FLUSH_LOG (bridge);
	}
}

unsigned short STP_GetMstConfigRevisionLevel (STP_BRIDGE* bridge)
{
	assert (bridge->ForceProtocolVersion >= STP_VERSION_MSTP);

	return bridge->MstConfigId.RevisionLevel;
}

// ============================================================================
/*
static void GetMstConfigDigest (STP_BRIDGE* bridge, const struct VLAN_TO_MSTID* table, unsigned int tableEntryCount, unsigned char digestOut [16])
{
	static const int EntriesInBlock = 16;

	INV_UINT2 mstids [EntriesInBlock];

	HMAC_MD5_CONTEXT context;
	HMAC_MD5_Init (&context);

	for (int vlan = 0; vlan < 4096; vlan += EntriesInBlock)
	{
		memset (mstids, 0, sizeof (mstids));

		for (unsigned int entryIndex = 0; entryIndex < tableEntryCount; entryIndex++)
		{
			unsigned short entryVlan = table [entryIndex].vlan;
			unsigned short entryMstid = table [entryIndex].mstid;

			assert (entryVlan > 0); // vlan0 is always assigned to the CIST, as per 802.1Q-2011, 13.7-4), so there shouldn't be any entry for it
			assert (entryVlan <= 4094);

			assert (entryMstid < bridge->treeCount);

			if ((entryVlan >= vlan) && (entryVlan < vlan + EntriesInBlock))
			{
				assert (mstids [entryVlan - vlan].GetValue () == 0); // trying to map the same vlan twice

				mstids [entryVlan - vlan] = entryMstid;
			}
		}

		HMAC_MD5_Update (&context, mstids, sizeof (mstids));
	}

	HMAC_MD5_End (&context);

	memcpy (digestOut, context.digest, 16);
}
*/
// ============================================================================
/*
void STP_SetMstConfigTable (STP_BRIDGE* bridge, const struct VLAN_TO_MSTID* table, unsigned int tableEntryCount)
{
	assert (bridge->ForceProtocolVersion >= STP_VERSION_MSTP);
	assert (tableEntryCount <= STP_MST_CONFIG_TABLE_ENTRY_COUNT_MAX);

	GetMstConfigDigest (bridge, table, tableEntryCount, bridge->MstConfigId.ConfigurationDigest);

	memcpy (bridge->mstConfigTable, table, tableEntryCount * sizeof (VLAN_TO_MSTID));
	bridge->mstConfigTableEntryCount = tableEntryCount;

	// we should assert BEGIN here, but we don't yet support setting the config id while the bridge is running
	assert (bridge->started == false);
}

const struct VLAN_TO_MSTID* STP_GetMstConfigTable (STP_BRIDGE* bridge, unsigned int* tableEntryCountOut)
{
	assert (bridge->ForceProtocolVersion >= STP_VERSION_MSTP);

	*tableEntryCountOut = bridge->mstConfigTableEntryCount;
	return bridge->mstConfigTable;
}
*/
void STP_GetMstConfigTable (STP_BRIDGE* bridge, unsigned char mstidsOut [4094])
{
	for (int i = 0; i < 4094; i++)
		mstidsOut [i] = (unsigned char) bridge->mstConfigTable [i + 1].GetValue ();
}

static void ComputeMstConfigDigest (STP_BRIDGE* bridge)
{
	HMAC_MD5_CONTEXT context;
	HMAC_MD5_Init (&context);
	HMAC_MD5_Update (&context, bridge->mstConfigTable, 2 * 4096);
	HMAC_MD5_End (&context);

	memcpy (bridge->MstConfigId.ConfigurationDigest, context.digest, 16);
}

void STP_SetMstConfigTableAndComputeDigest1 (STP_BRIDGE* bridge, const unsigned char mstids [4094], unsigned int timestamp)
{
	assert (bridge->ForceProtocolVersion >= STP_VERSION_MSTP);

	LOG (bridge, -1, -1, "{T}: Setting MST Config Table...\r\n", timestamp);

	for (int i = 0; i < 4094; i++)
	{
		unsigned char mstid = mstids [i];

		// Check that the caller is not trying to map a VLAN to a too-large tree number.
		assert (mstid < bridge->treeCount);

		// write it in BE format
		bridge->mstConfigTable [i + 1] = mstid;
	}

	ComputeMstConfigDigest (bridge);

	if (bridge->started)
	{
		bridge->BEGIN = true;
		RunStateMachines (bridge, timestamp);
		bridge->BEGIN = false;
		RunStateMachines (bridge, timestamp);
	}

	LOG (bridge, -1, -1, "------------------------------------\r\n");
	FLUSH_LOG (bridge);
}

void STP_SetMstConfigTableAndComputeDigest (STP_BRIDGE* bridge, const struct VLAN_TO_MSTID* table, unsigned int tableEntryCount, unsigned int timestamp)
{
	assert (bridge->ForceProtocolVersion >= STP_VERSION_MSTP);

	LOG (bridge, -1, -1, "{T}: Setting MST Config Table...\r\n", timestamp);

	memset (bridge->mstConfigTable, 0, sizeof bridge->mstConfigTable);
	for (unsigned int i = 0; i < tableEntryCount; i++)
	{
		const VLAN_TO_MSTID* entry = &table [i];
		unsigned short vlan = entry->vlanLow | (entry->vlanHigh << 8);
		assert ((vlan > 0) && (vlan <= 4094));

		// Check that the caller is not trying to map a VLAN to a too-large tree number.
		assert (entry->mstid <= bridge->treeCount);

		assert (bridge->mstConfigTable [vlan] == (unsigned short) 0); // trying to set twice the mstid for the same vlan

		bridge->mstConfigTable [vlan] = entry->mstid;
	}

	ComputeMstConfigDigest (bridge);

	if (bridge->started)
	{
		bridge->BEGIN = true;
		RunStateMachines (bridge, timestamp);
		bridge->BEGIN = false;
		RunStateMachines (bridge, timestamp);
	}

	LOG (bridge, -1, -1, "------------------------------------\r\n");
	FLUSH_LOG (bridge);
}

const unsigned char* STP_GetMstConfigTableDigest (STP_BRIDGE* bridge)
{
	assert (bridge->ForceProtocolVersion >= STP_VERSION_MSTP);

	return bridge->MstConfigId.ConfigurationDigest;
}

// ============================================================================

unsigned int STP_GetPortCount (STP_BRIDGE* bridge)
{
	return bridge->portCount;
}

unsigned int STP_GetTreeCount (STP_BRIDGE* bridge)
{
	return bridge->treeCount;
}

enum STP_VERSION STP_GetStpVersion (STP_BRIDGE* bridge)
{
	return bridge->ForceProtocolVersion;
}

void STP_SetStpVersion (STP_BRIDGE* bridge, enum STP_VERSION version)
{
	assert (false); // not implemented
}

bool STP_GetPortEnabled (STP_BRIDGE* bridge, unsigned int portIndex)
{
	return bridge->ports [portIndex]->portEnabled;
}

STP_PORT_ROLE STP_GetPortRole (STP_BRIDGE* bridge, unsigned int portIndex, unsigned int treeIndex)
{
	return bridge->ports [portIndex]->trees [treeIndex]->role;
}

bool STP_GetPortLearning (STP_BRIDGE* bridge, unsigned int portIndex, unsigned int treeIndex)
{
	return bridge->ports [portIndex]->trees [treeIndex]->learning;
}

bool STP_GetPortForwarding (STP_BRIDGE* bridge, unsigned int portIndex, unsigned int treeIndex)
{
	return bridge->ports [portIndex]->trees [treeIndex]->forwarding;
}

bool STP_GetPortOperEdge (STP_BRIDGE* bridge, unsigned int portIndex)
{
	return bridge->ports [portIndex]->operEdge;
}

bool STP_GetPortOperPointToPointMAC (STP_BRIDGE* bridge, unsigned int portIndex)
{
	return bridge->ports [portIndex]->operPointToPointMAC;
}

unsigned char STP_GetTreeIndexFromVlanNumber (STP_BRIDGE* bridge, unsigned short vlanNumber)
{
	assert ((vlanNumber >= 1) && (vlanNumber <= 4094));

	return (unsigned char) bridge->mstConfigTable [vlanNumber].GetValue ();
}

// ============================================================================

const char* STP_GetPortRoleString (STP_PORT_ROLE portRole)
{
	switch (portRole)
	{
		case STP_PORT_ROLE_DISABLED:	return "Disabled";
		case STP_PORT_ROLE_ROOT:		return "Root";
		case STP_PORT_ROLE_DESIGNATED:	return "Designated";
		case STP_PORT_ROLE_ALTERNATE:	return "Alternate";
		case STP_PORT_ROLE_BACKUP:		return "Backup";
		case STP_PORT_ROLE_MASTER:		return "Master";
		default:						return "(unknown)";
	}
}

const char* STP_GetVersionString (enum STP_VERSION version)
{
	switch (version)
	{
		case STP_VERSION_LEGACY_STP:	return "LegacySTP";
		case STP_VERSION_RSTP:			return "RSTP";
		case STP_VERSION_MSTP:			return "MSTP";
		default:						return "(unknown)";
	}
}

const char* STP_GetAdminP2PString (enum STP_ADMIN_P2P adminP2P)
{
	switch (adminP2P)
	{
		case STP_ADMIN_P2P_AUTO:        return "Auto";
		case STP_ADMIN_P2P_FORCE_TRUE:  return "ForceTrue";
		case STP_ADMIN_P2P_FORCE_FALSE: return "ForceFalse";
		default: return "(unknown)";
	}
}

// ============================================================================

void STP_GetRootPriorityVector (STP_BRIDGE* bridge, unsigned int treeIndex, unsigned char* priorityVector36BytesOut)
{
	const unsigned char* rootPriority = (const unsigned char*) &bridge->trees [treeIndex]->rootPriority;
	const unsigned char* rootPortId   = (const unsigned char*) &bridge->trees [treeIndex]->rootPortId;
	memcpy (priorityVector36BytesOut, rootPriority, 34);
	priorityVector36BytesOut [34] = rootPortId [0];
	priorityVector36BytesOut [35] = rootPortId [1];
}

void STP_GetRootTimes (STP_BRIDGE* bridge,
					   unsigned int treeIndex,
					   unsigned short* forwardDelayOutOrNull,
					   unsigned short* helloTimeOutOrNull,
					   unsigned short* maxAgeOutOrNull,
					   unsigned short* messageAgeOutOrNull,
					   unsigned char* remainingHopsOutOrNull)
{
	// This retrieves the rootTimes variable described in 13.24.9 in 802.1Q-2011.
	// These values are meaningful only as long as the bridge is running, hence the following assert.
	assert (bridge->started);

	assert (treeIndex < bridge->treeCount);
	BRIDGE_TREE* tree = bridge->trees [treeIndex];

	if (forwardDelayOutOrNull != NULL)
		*forwardDelayOutOrNull = tree->rootTimes.ForwardDelay;

	if (helloTimeOutOrNull != NULL)
		*helloTimeOutOrNull = tree->rootTimes.HelloTime;

	if (maxAgeOutOrNull != NULL)
		*maxAgeOutOrNull = tree->rootTimes.MaxAge;

	if (messageAgeOutOrNull != NULL)
		*messageAgeOutOrNull = tree->rootTimes.MessageAge;

	if (remainingHopsOutOrNull != NULL)
		*remainingHopsOutOrNull = tree->rootTimes.remainingHops;
}

// ============================================================================

void  STP_SetApplicationContext (STP_BRIDGE* bridge, void* applicationContext)
{
	bridge->applicationContext = applicationContext;
}

// ============================================================================

void* STP_GetApplicationContext (STP_BRIDGE* bridge)
{
	return bridge->applicationContext;
}