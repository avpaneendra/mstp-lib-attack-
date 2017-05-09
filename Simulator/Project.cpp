
#include "pch.h"
#include "Simulator.h"
#include "Wire.h"
#include "Bridge.h"
#include "Port.h"

#pragma comment (lib, "msxml6.lib")

using namespace std;

class Project : public IProject
{
	ULONG _refCount = 1;
	vector<ComPtr<Bridge>> _bridges;
	vector<ComPtr<Wire>> _wires;
	EventManager _em;
	std::array<uint8_t, 6> _nextMacAddress = { 0x00, 0xAA, 0x55, 0xAA, 0x55, 0x80 };

public:
	virtual const vector<ComPtr<Bridge>>& GetBridges() const override final { return _bridges; }

	virtual void InsertBridge (size_t index, Bridge* bridge) override final
	{
		if (index > _bridges.size())
			throw invalid_argument("index");

		_bridges.push_back(bridge);
		bridge->GetInvalidateEvent().AddHandler (&OnObjectInvalidate, this);
		BridgeInsertedEvent::InvokeHandlers (_em, this, index, bridge);
		ProjectInvalidateEvent::InvokeHandlers (_em, this);
	}

	virtual void RemoveBridge(size_t index) override final
	{
		if (index >= _bridges.size())
			throw invalid_argument("index");

		auto& b = _bridges[index];

		size_t wireIndex = 0;
		while (wireIndex < _wires.size())
		{
			Wire* w = _wires[wireIndex].Get();

			bool allWirePointsOnBridgeBeingDeleted = all_of (w->GetPoints().begin(), w->GetPoints().end(),
				[&b](const WireEnd& we) { return holds_alternative<ConnectedWireEnd>(we) && (get<ConnectedWireEnd>(we)->GetBridge() == b); });

			if (allWirePointsOnBridgeBeingDeleted)
				this->RemoveWire(wireIndex);
			else
			{
				for (size_t i = 0; i < w->GetPoints().size(); i++)
				{
					auto& point = w->GetPoints()[i];
					if (holds_alternative<ConnectedWireEnd>(point) && (get<ConnectedWireEnd>(point)->GetBridge() == b))
						w->SetPoint(i, w->GetPointCoords(i));
				}
		
				wireIndex++;
			}
		}

		BridgeRemovingEvent::InvokeHandlers(_em, this, index, _bridges[index]);
		_bridges[index]->GetInvalidateEvent().RemoveHandler (&OnObjectInvalidate, this);
		_bridges.erase (_bridges.begin() + index);
		ProjectInvalidateEvent::InvokeHandlers (_em, this);
	}

	virtual const vector<ComPtr<Wire>>& GetWires() const override final { return _wires; }

	virtual void InsertWire (size_t index, Wire* wire) override final
	{
		if (index > _wires.size())
			throw invalid_argument("index");

		_wires.push_back(wire);
		wire->GetInvalidateEvent().AddHandler (&OnObjectInvalidate, this);
		WireInsertedEvent::InvokeHandlers (_em, this, index, wire);
		ProjectInvalidateEvent::InvokeHandlers (_em, this);
	}

	virtual void RemoveWire (size_t index) override final
	{
		if (index >= _wires.size())
			throw invalid_argument("index");

		WireRemovingEvent::InvokeHandlers(_em, this, index, _wires[index]);
		_wires[index]->GetInvalidateEvent().RemoveHandler (&OnObjectInvalidate, this);
		_wires.erase(_wires.begin() + index);
		ProjectInvalidateEvent::InvokeHandlers (_em, this);
	}

	static void OnObjectInvalidate (void* callbackArg, Object* object)
	{
		auto project = static_cast<Project*>(callbackArg);
		ProjectInvalidateEvent::InvokeHandlers (project->_em, project);
	}

	virtual BridgeInsertedEvent::Subscriber GetBridgeInsertedEvent() override final { return BridgeInsertedEvent::Subscriber(_em); }
	virtual BridgeRemovingEvent::Subscriber GetBridgeRemovingEvent() override final { return BridgeRemovingEvent::Subscriber(_em); }

	virtual WireInsertedEvent::Subscriber GetWireInsertedEvent() override final { return WireInsertedEvent::Subscriber(_em); }
	virtual WireRemovingEvent::Subscriber GetWireRemovingEvent() override final { return WireRemovingEvent::Subscriber(_em); }

	virtual ProjectInvalidateEvent::Subscriber GetProjectInvalidateEvent() override final { return ProjectInvalidateEvent::Subscriber(_em); }

	virtual array<uint8_t, 6> AllocMacAddressRange (size_t count) override final
	{
		if (count >= 128)
			throw range_error("count must be lower than 128.");

		auto result = _nextMacAddress;
		_nextMacAddress[5] += (uint8_t)count;
		if (_nextMacAddress[5] < count)
		{
			_nextMacAddress[4]++;
			if (_nextMacAddress[4] == 0)
				throw not_implemented_exception();
		}

		return result;
	}

	virtual void Save (const wchar_t* path) const override final
	{
		ComPtr<IXMLDOMDocument3> doc;
		HRESULT hr = CoCreateInstance (CLSID_DOMDocument60, nullptr, CLSCTX_INPROC_SERVER, __uuidof(doc), (void**) &doc); ThrowIfFailed(hr);

		ComPtr<IXMLDOMElement> projectElement;
		hr = doc->createElement (_bstr_t("Project"), &projectElement); ThrowIfFailed(hr);
		hr = doc->appendChild (projectElement, nullptr); ThrowIfFailed(hr);

		ComPtr<IXMLDOMElement> bridgesElement;
		hr = doc->createElement (_bstr_t("Bridges"), &bridgesElement); ThrowIfFailed(hr);
		hr = projectElement->appendChild (bridgesElement, nullptr); ThrowIfFailed(hr);
		for (auto& b : _bridges)
		{
			auto e = b->Serialize(doc);
			hr = bridgesElement->appendChild (e, nullptr); ThrowIfFailed(hr);
		}

		ComPtr<IXMLDOMElement> wiresElement;
		hr = doc->createElement (_bstr_t("Wires"), &wiresElement); ThrowIfFailed(hr);
		hr = projectElement->appendChild (wiresElement, nullptr); ThrowIfFailed(hr);
		for (auto& w : _wires)
		{
			hr = wiresElement->appendChild (w->Serialize(doc), nullptr);
			ThrowIfFailed(hr);
		}

		FormatAndSaveToFile (doc, path);
	}

	void FormatAndSaveToFile (IXMLDOMDocument3* doc, const wchar_t* path) const
	{
		static const char StylesheetText[] =
			"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
			"<xsl:stylesheet xmlns:xsl=\"http://www.w3.org/1999/XSL/Transform\" version=\"1.0\">\n"
			"  <xsl:output method=\"xml\" indent=\"yes\" omit-xml-declaration=\"no\" />\n"
			"  <xsl:template match=\"@* | node()\">\n"
			"    <xsl:copy>\n"
			"      <xsl:apply-templates select=\"@* | node()\"/>\n"
			"    </xsl:copy>\n"
			"  </xsl:template>\n"
			"</xsl:stylesheet>\n"
			"";

		ComPtr<IXMLDOMDocument3> loadXML;
		HRESULT hr = CoCreateInstance (CLSID_DOMDocument60, nullptr, CLSCTX_INPROC_SERVER, __uuidof(loadXML), (void**) &loadXML); ThrowIfFailed(hr);
		VARIANT_BOOL successful;
		hr = loadXML->loadXML (_bstr_t(StylesheetText), &successful); ThrowIfFailed(hr);

		//Create the final document which will be indented properly
		ComPtr<IXMLDOMDocument2> pXMLFormattedDoc;
		hr = pXMLFormattedDoc.CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER);

		ComPtr<IDispatch> pDispatch;
		hr = pXMLFormattedDoc->QueryInterface(IID_IDispatch, (void**)&pDispatch); ThrowIfFailed(hr);

		_variant_t vtOutObject;
		vtOutObject.vt = VT_DISPATCH;
		vtOutObject.pdispVal = pDispatch;
		vtOutObject.pdispVal->AddRef();

		//Apply the transformation to format the final document	
		hr = doc->transformNodeToObject(loadXML,vtOutObject);

		// By default it is writing the encoding = UTF-16. Let us change the encoding to UTF-8
		ComPtr<IXMLDOMNode> firstChild;
		hr = pXMLFormattedDoc->get_firstChild(&firstChild); ThrowIfFailed(hr);
		ComPtr<IXMLDOMNamedNodeMap> pXMLAttributeMap;
		hr = firstChild->get_attributes(&pXMLAttributeMap); ThrowIfFailed(hr);
		ComPtr<IXMLDOMNode> encodingNode;
		hr = pXMLAttributeMap->getNamedItem(_bstr_t("encoding"), &encodingNode); ThrowIfFailed(hr);
		encodingNode->put_nodeValue (_variant_t("UTF-8"));

		hr = pXMLFormattedDoc->save(_variant_t(path)); ThrowIfFailed(hr);
	}

	#pragma region IUnknown
	virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override final { return E_NOTIMPL; }

	virtual ULONG STDMETHODCALLTYPE AddRef() override final
	{
		return InterlockedIncrement(&_refCount);
	}

	virtual ULONG STDMETHODCALLTYPE Release() override final
	{
		auto newRefCount = InterlockedDecrement (&_refCount);
		if (newRefCount == 0)
			delete this;
		return newRefCount;
	}
	#pragma endregion
};

extern const ProjectFactory projectFactory = [] { return ComPtr<IProject>(new Project(), false); };
