#include "pch.h"
#include "Bridge.h"
#include "Win32Defs.h"
#include "Port.h"
#include "Wire.h"

using namespace std;
using namespace D2D1;

static constexpr UINT WM_ONE_SECOND_TIMER = WM_APP + 1;
static constexpr UINT WM_MAC_OPERATIONAL_TIMER = WM_APP + 2;
static constexpr UINT WM_PACKET_RECEIVED = WM_APP + 3;

static constexpr uint8_t BpduDestAddress[6] = { 1, 0x80, 0xC2, 0, 0, 0 };

Bridge::Bridge (IProject* project, unsigned int portCount, const std::array<uint8_t, 6>& macAddress)
	: _project(project), _macAddress(macAddress), _guiThreadId(this_thread::get_id())
{
	float offset = 0;

	for (size_t i = 0; i < portCount; i++)
	{
		offset += (Port::PortToPortSpacing / 2 + Port::InteriorLongSize / 2);
		auto port = ComPtr<Port>(new Port(this, i, Side::Bottom, offset), false);
		_ports.push_back (move(port));
		offset += (Port::InteriorLongSize / 2 + Port::PortToPortSpacing / 2);
	}

	_x = 0;
	_y = 0;
	_width = max (offset, MinWidth);
	_height = DefaultHeight;

	HINSTANCE hInstance;
	BOOL bRes = GetModuleHandleEx (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR) &StpCallbacks, &hInstance);
	if (!bRes)
		throw win32_exception(GetLastError());

	auto hwnd = CreateWindow (L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, hInstance, 0);
	if (hwnd == nullptr)
		throw win32_exception(GetLastError());
	_helperWindow.reset (hwnd);

	bRes = SetWindowSubclass (hwnd, HelperWindowProc, 0, 0);
	if (!bRes)
		throw win32_exception(GetLastError());

	DWORD period = 950 + (std::random_device()() % 100);
	HANDLE handle;
	bRes = CreateTimerQueueTimer (&handle, nullptr, OneSecondTimerCallback, this, period, period, 0);
	if (!bRes)
		throw win32_exception(GetLastError());
	_oneSecondTimerHandle.reset(handle);
	
	period = 45 + (std::random_device()() % 10);
	bRes = CreateTimerQueueTimer (&handle, nullptr, MacOperationalTimerCallback, this, period, period, 0);
	if (!bRes)
		throw win32_exception(GetLastError());
	_macOperationalTimerHandle.reset(handle);
}

Bridge::~Bridge()
{
	// First stop the timers, to be sure the mutex won't be acquired in a background thread (when we'll have background threads).
	_macOperationalTimerHandle = nullptr;
	_oneSecondTimerHandle = nullptr;

	RemoveWindowSubclass (_helperWindow.get(), HelperWindowProc, 0);

	if (_stpBridge != nullptr)
		STP_DestroyBridge (_stpBridge);
}

//static
void CALLBACK Bridge::OneSecondTimerCallback (void* lpParameter, BOOLEAN TimerOrWaitFired)
{
	auto bridge = static_cast<Bridge*>(lpParameter);
	::PostMessage (bridge->_helperWindow.get(), WM_ONE_SECOND_TIMER, (WPARAM) bridge, 0);
}

//static
void CALLBACK Bridge::MacOperationalTimerCallback (void* lpParameter, BOOLEAN TimerOrWaitFired)
{
	auto bridge = static_cast<Bridge*>(lpParameter);
	::PostMessage (bridge->_helperWindow.get(), WM_MAC_OPERATIONAL_TIMER, (WPARAM) bridge, 0);
}

// static
LRESULT CALLBACK Bridge::HelperWindowProc (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	if (uMsg == WM_ONE_SECOND_TIMER)
	{
		auto bridge = static_cast<Bridge*>((void*)wParam);

		if (bridge->_stpBridge != nullptr)
			STP_OnOneSecondTick (bridge->_stpBridge, GetTimestampMilliseconds());

		return 0;
	}
	else if (uMsg == WM_MAC_OPERATIONAL_TIMER)
	{
		auto bridge = static_cast<Bridge*>((void*)wParam);
		bridge->ComputeMacOperational();
		return 0;
	}
	else if (uMsg == WM_PACKET_RECEIVED)
	{
		auto bridge = static_cast<Bridge*>((void*)wParam);
		bridge->ProcessReceivedPacket();
		return 0;
	}

	return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

void Bridge::ComputeMacOperational()
{
	assert (this_thread::get_id() == _guiThreadId);
	
	auto timestamp = GetTimestampMilliseconds();

	bool invalidate = false;
	for (size_t portIndex = 0; portIndex < _ports.size(); portIndex++)
	{
		auto& port = _ports[portIndex];
	
		bool newMacOperational = (_project->FindReceivingPort(port) != nullptr);
		if (port->_macOperational != newMacOperational)
		{
			if (port->_macOperational)
			{
				// port just disconnected
				if (_stpBridge)
					STP_OnPortDisabled (_stpBridge, portIndex, timestamp);
			}

			port->_macOperational = newMacOperational;

			if (port->_macOperational)
			{
				// port just connected
				if (_stpBridge)
					STP_OnPortEnabled (_stpBridge, portIndex, 100, true, timestamp);
			}

			invalidate = true;
		}
	}

	if (invalidate)
		InvalidateEvent::InvokeHandlers(_em, this);
}

void Bridge::ProcessReceivedPacket()
{
	auto rp = move(_rxQueue.front());
	_rxQueue.pop();
	
	if (memcmp (&rp.data[0], BpduDestAddress, 6) == 0)
	{
		// It's a BPDU.
		if (_stpBridge != nullptr)
		{
			if (!_ports[rp.portIndex]->_macOperational)
			{
				_ports[rp.portIndex]->_macOperational = true;
				STP_OnPortEnabled (_stpBridge, rp.portIndex, 100, true, rp.timestamp);
				InvalidateEvent::InvokeHandlers (_em, this);
			}

			STP_OnBpduReceived (_stpBridge, rp.portIndex, &rp.data[21], (unsigned int) (rp.data.size() - 21), rp.timestamp);
		}
		else
		{
			// broadcast it to the other ports.
			for (auto& txPort : _ports)
			{
				if (txPort->_portIndex != rp.portIndex)
				{
					auto rxPort = _project->FindReceivingPort(txPort);
					if (rxPort != nullptr)
					{
						RxPacketInfo info = { rp.data, rxPort->_portIndex, rp.timestamp };
						rxPort->_bridge->_rxQueue.push(move(info));
						::PostMessage (rxPort->_bridge->_helperWindow.get(), WM_PACKET_RECEIVED, (WPARAM)(void*)rxPort->_bridge, 0);
					}
				}
			}
		}
	}
}

void Bridge::EnableStp (STP_VERSION stpVersion, uint16_t treeCount, uint32_t timestamp)
{
	if (this_thread::get_id() != _guiThreadId)
		throw runtime_error ("This function may be called only on the main thread.");

	if (_stpBridge != nullptr)
		throw runtime_error ("STP is already enabled on this bridge.");

	_stpBridge = STP_CreateBridge ((unsigned int) _ports.size(), treeCount, &StpCallbacks, stpVersion, &_macAddress[0], 256);
	STP_SetApplicationContext (_stpBridge, this);
	STP_EnableLogging (_stpBridge, true);
	STP_StartBridge (_stpBridge, timestamp);
	BridgeStartedEvent::InvokeHandlers (_em, this);

	for (size_t portIndex = 0; portIndex < _ports.size(); portIndex++)
	{
		auto& port = _ports[portIndex];
		if (_project->FindReceivingPort(port) != nullptr)
			STP_OnPortEnabled (_stpBridge, portIndex, 100, true, timestamp);
	}

	InvalidateEvent::InvokeHandlers(_em, this);
}

void Bridge::DisableStp (uint32_t timestamp)
{
	if (this_thread::get_id() != _guiThreadId)
		throw runtime_error ("This function may be called only on the main thread.");

	if (_stpBridge == nullptr)
		throw runtime_error ("STP was not enabled on this bridge.");

	BridgeStoppingEvent::InvokeHandlers(_em, this);
	STP_StopBridge(_stpBridge, timestamp);
	STP_DestroyBridge (_stpBridge);
	_stpBridge = nullptr;

	InvalidateEvent::InvokeHandlers(_em, this);
}

void Bridge::SetLocation(float x, float y)
{
	if ((_x != x) || (_y != y))
	{
		InvalidateEvent::InvokeHandlers(_em, this);
		_x = x;
		_y = y;
		InvalidateEvent::InvokeHandlers(_em, this);
	}
}

uint16_t Bridge::GetTreeCount() const
{
	if (_stpBridge == nullptr)
		throw runtime_error ("STP was not enabled on this bridge.");

	return STP_GetTreeCount(_stpBridge);
}

STP_PORT_ROLE Bridge::GetStpPortRole (uint16_t portIndex, uint16_t treeIndex) const
{
	if (_stpBridge == nullptr)
		throw runtime_error ("STP was not enabled on this bridge.");

	return STP_GetPortRole (_stpBridge, portIndex, treeIndex);
}

bool Bridge::GetStpPortLearning (uint16_t portIndex, uint16_t treeIndex) const
{
	if (_stpBridge == nullptr)
		throw runtime_error ("STP was not enabled on this bridge.");

	return STP_GetPortLearning (_stpBridge, portIndex, treeIndex);
}

bool Bridge::GetStpPortForwarding (uint16_t portIndex, uint16_t treeIndex) const
{
	if (_stpBridge == nullptr)
		throw runtime_error ("STP was not enabled on this bridge.");

	return STP_GetPortForwarding (_stpBridge, portIndex, treeIndex);
}

bool Bridge::GetStpPortOperEdge (uint16_t portIndex) const
{
	if (_stpBridge == nullptr)
		throw runtime_error ("STP was not enabled on this bridge.");

	return STP_GetPortOperEdge (_stpBridge, portIndex);
}

unsigned short Bridge::GetStpBridgePriority (uint16_t treeIndex) const
{
	if (_stpBridge == nullptr)
		throw runtime_error ("STP was not enabled on this bridge.");

	return STP_GetBridgePriority(_stpBridge, treeIndex);
}

uint16_t Bridge::GetStpTreeIndexFromVlanNumber (uint16_t vlanNumber) const
{
	if (_stpBridge == nullptr)
		throw runtime_error ("STP was not enabled on this bridge.");

	if ((vlanNumber == 0) || (vlanNumber > 4094))
		throw invalid_argument ("The VLAN number must be >=1 and <=4094.");

	return STP_GetTreeIndexFromVlanNumber(_stpBridge, vlanNumber);
}

void Bridge::Render (ID2D1RenderTarget* dc, const DrawingObjects& dos, IDWriteFactory* dWriteFactory, uint16_t vlanNumber) const
{
	optional<unsigned int> treeIndex;
	if (IsStpEnabled())
		treeIndex = GetStpTreeIndexFromVlanNumber(vlanNumber);

	bool isRootBridge = ((_stpBridge != nullptr) && STP_IsRootBridge(_stpBridge));
	// Draw bridge outline.
	D2D1_ROUNDED_RECT rr = RoundedRect (GetBounds(), RoundRadius, RoundRadius);
	dc->FillRoundedRectangle (&rr, _powered ? dos._poweredFillBrush : dos._unpoweredBrush);
	float ow = isRootBridge ? 5.0f : 2.0f;
	dc->DrawRoundedRectangle (&rr, dos._brushWindowText, ow);

	// Draw bridge name.
	wchar_t str[128];
	int strlen;
	if (IsStpEnabled())
	{
		unsigned short prio = GetStpBridgePriority(treeIndex.value());
		strlen = swprintf_s (str, L"%04x.%02x%02x%02x%02x%02x%02x\r\nSTP enabled\r\n%s", prio,
							 _macAddress[0], _macAddress[1], _macAddress[2], _macAddress[3], _macAddress[4], _macAddress[5],
							 isRootBridge ? L"Root Bridge\r\n" : L"");
	}
	else
	{
		strlen = swprintf_s (str, L"%02x%02x%02x%02x%02x%02x\r\nSTP disabled (right-click to enable)",
			_macAddress[0], _macAddress[1], _macAddress[2], _macAddress[3], _macAddress[4], _macAddress[5]);
	}
	ComPtr<IDWriteTextLayout> tl;
	HRESULT hr = dWriteFactory->CreateTextLayout (str, strlen, dos._regularTextFormat, 10000, 10000, &tl); ThrowIfFailed(hr);
	dc->DrawTextLayout ({ _x + OutlineWidth / 2 + 3, _y + OutlineWidth / 2 + 3}, tl, dos._brushWindowText);

	Matrix3x2F oldTransform;
	dc->GetTransform (&oldTransform);

	for (auto& port : _ports)
		port->Render (dc, dos, dWriteFactory, vlanNumber);
}

void Bridge::RenderSelection (const IZoomable* zoomable, ID2D1RenderTarget* rt, const DrawingObjects& dos) const
{
	auto oldaa = rt->GetAntialiasMode();
	rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

	auto tl = zoomable->GetDLocationFromWLocation ({ _x - OutlineWidth / 2, _y - OutlineWidth / 2 });
	auto br = zoomable->GetDLocationFromWLocation ({ _x + _width + OutlineWidth / 2, _y + _height + OutlineWidth / 2 });
	rt->DrawRectangle ({ tl.x - 10, tl.y - 10, br.x + 10, br.y + 10 }, dos._brushHighlight, 2, dos._strokeStyleSelectionRect);

	rt->SetAntialiasMode(oldaa);
}

HTResult Bridge::HitTest (const IZoomable* zoomable, D2D1_POINT_2F dLocation, float tolerance)
{
	for (auto& p : _ports)
	{
		auto ht = p->HitTest (zoomable, dLocation, tolerance);
		if (ht.object != nullptr)
			return ht;
	}

	auto tl = zoomable->GetDLocationFromWLocation ({ _x, _y });
	auto br = zoomable->GetDLocationFromWLocation ({ _x + _width, _y + _height });
	
	if ((dLocation.x >= tl.x) && (dLocation.y >= tl.y) && (dLocation.x < br.x) && (dLocation.y < br.y))
		return { this, HTCodeInner };
	
	return {};
}

bool Bridge::IsPortForwardingOnVlan (unsigned int portIndex, uint16_t vlanNumber) const
{
	if (_stpBridge == nullptr)
		return true;

	auto treeIndex = STP_GetTreeIndexFromVlanNumber(_stpBridge, vlanNumber);
	return STP_GetPortForwarding(_stpBridge, portIndex, treeIndex);
}

bool Bridge::IsStpRootBridge() const
{
	if (_stpBridge == nullptr)
		throw runtime_error ("STP was not enabled on this bridge.");

	return STP_IsRootBridge(_stpBridge);
}

#pragma region STP Callbacks
const STP_CALLBACKS Bridge::StpCallbacks =
{
	StpCallback_EnableLearning,
	StpCallback_EnableForwarding,
	StpCallback_TransmitGetBuffer,
	StpCallback_TransmitReleaseBuffer,
	StpCallback_FlushFdb,
	StpCallback_DebugStrOut,
	StpCallback_OnTopologyChange,
	StpCallback_OnNotifiedTopologyChange,
	StpCallback_AllocAndZeroMemory,
	StpCallback_FreeMemory,
};


void* Bridge::StpCallback_AllocAndZeroMemory(unsigned int size)
{
	void* p = malloc(size);
	memset (p, 0, size);
	return p;
}

void Bridge::StpCallback_FreeMemory(void* p)
{
	free(p);
}

void* Bridge::StpCallback_TransmitGetBuffer (STP_BRIDGE* bridge, unsigned int portIndex, unsigned int bpduSize, unsigned int timestamp)
{
	auto b = static_cast<Bridge*>(STP_GetApplicationContext(bridge));
	auto txPort = b->_ports[portIndex].Get();
	auto rxPort = b->_project->FindReceivingPort(txPort);
	if (rxPort == nullptr)
		return nullptr; // The port was disconnected and our port polling code hasn't reacted yet. This is what happens in a real system too.

	b->_txPacketData.resize (bpduSize + 21);
	memcpy (&b->_txPacketData[0], BpduDestAddress, 6);
	b->_txReceivingPort = rxPort;
	b->_txTimestamp = timestamp;
	return &b->_txPacketData[21];
}

void Bridge::StpCallback_TransmitReleaseBuffer (STP_BRIDGE* bridge, void* bufferReturnedByGetBuffer)
{
	auto transmittingBridge = static_cast<Bridge*>(STP_GetApplicationContext(bridge));
	
	RxPacketInfo info;
	info.data = move(transmittingBridge->_txPacketData);
	info.portIndex = transmittingBridge->_txReceivingPort->_portIndex;
	info.timestamp = transmittingBridge->_txTimestamp;

	Bridge* receivingBridge = transmittingBridge->_txReceivingPort->_bridge;
	receivingBridge->_rxQueue.push (move(info));
	::PostMessage (receivingBridge->_helperWindow.get(), WM_PACKET_RECEIVED, (WPARAM)(void*)receivingBridge, 0);
}

void Bridge::StpCallback_EnableLearning(STP_BRIDGE* bridge, unsigned int portIndex, unsigned int treeIndex, bool enable)
{
	auto b = static_cast<Bridge*>(STP_GetApplicationContext(bridge));
	InvalidateEvent::InvokeHandlers (b->_em, b);
}

void Bridge::StpCallback_EnableForwarding(STP_BRIDGE* bridge, unsigned int portIndex, unsigned int treeIndex, bool enable)
{
	auto b = static_cast<Bridge*>(STP_GetApplicationContext(bridge));
	InvalidateEvent::InvokeHandlers(b->_em, b);
}

void Bridge::StpCallback_FlushFdb (STP_BRIDGE* bridge, unsigned int portIndex, unsigned int treeIndex, enum STP_FLUSH_FDB_TYPE flushType)
{
	auto b = static_cast<Bridge*>(STP_GetApplicationContext(bridge));
}

void Bridge::StpCallback_DebugStrOut (STP_BRIDGE* bridge, int portIndex, int treeIndex, const char* nullTerminatedString, unsigned int stringLength, bool flush)
{
	auto b = static_cast<Bridge*>(STP_GetApplicationContext(bridge));

	if (this_thread::get_id() != b->_guiThreadId)
		throw std::runtime_error("Logging-related code does not yet support multithreading.");

	if (stringLength > 0)
	{
		if (b->_currentLogLine.text.empty())
		{
			b->_currentLogLine.text.assign (nullTerminatedString, (size_t) stringLength);
			b->_currentLogLine.portIndex = portIndex;
			b->_currentLogLine.treeIndex = treeIndex;
		}
		else
		{
			if ((b->_currentLogLine.portIndex != portIndex) || (b->_currentLogLine.treeIndex != treeIndex))
			{
				b->_logLines.push_back(move(b->_currentLogLine));
				BridgeLogLineGenerated::InvokeHandlers (b->_em, b, b->_logLines.back());
			}

			b->_currentLogLine.text.append (nullTerminatedString, (size_t) stringLength);
		}

		if (!b->_currentLogLine.text.empty() && (b->_currentLogLine.text.back() == L'\n'))
		{
			b->_logLines.push_back(move(b->_currentLogLine));
			BridgeLogLineGenerated::InvokeHandlers (b->_em, b, b->_logLines.back());
		}
	}

	if (flush && !b->_currentLogLine.text.empty())
	{
		b->_logLines.push_back(move(b->_currentLogLine));
		BridgeLogLineGenerated::InvokeHandlers (b->_em, b, b->_logLines.back());
	}
}

void Bridge::StpCallback_OnTopologyChange (STP_BRIDGE* bridge)
{
}

void Bridge::StpCallback_OnNotifiedTopologyChange (STP_BRIDGE* bridge, unsigned int portIndex, unsigned int treeIndex)
{
}
#pragma endregion

