
#include "pch.h"
#include "Simulator.h"
#include "Bridge.h"
#include "Wire.h"
#include "win32/property_grid.h"
#include "win32/window.h"

using namespace std;
using namespace edge;

static const wchar_t WndClassName[] = L"properties_window-{6ED5A45A-9BF5-4EA2-9F43-4EFEDC11994E}";

#pragma warning (disable: 4250)

class properties_window : public window, public virtual properties_window_i
{
	using base = window;

	selection_i*     const _selection;
	IProjectWindow* const _project_window;
	IProject*       const _project;
	unique_ptr<property_grid_i> _pg1;

public:
	properties_window (simulator_app_i* app,
					  IProjectWindow* projectWindow,
					  IProject* project,
					  selection_i* selection,
					  const RECT& rect,
					  HWND hWndParent,
					  ID3D11DeviceContext1* d3d_dc,
					  IDWriteFactory* dwrite_factory)
		: base (app->GetHInstance(), WS_EX_CLIENTEDGE, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, rect, hWndParent, 0)
		, _project_window(projectWindow)
		, _project(project)
		, _selection(selection)
		, _pg1(property_grid_factory(app->GetHInstance(), client_rect_pixels(), hwnd(), d3d_dc, dwrite_factory))
	{
		this->set_selection_to_pgs();

		_project_window->selected_vlan_number_changed().add_handler (&on_selected_vlan_changed, this);
		_selection->changed().add_handler (&on_selection_changed, this);
		_project->GetChangedEvent().add_handler (&on_project_changed, this);
	}

	virtual ~properties_window()
	{
		_project->GetChangedEvent().remove_handler (&on_project_changed, this);
		_selection->changed().remove_handler (&on_selection_changed, this);
		_project_window->selected_vlan_number_changed().remove_handler (&on_selected_vlan_changed, this);
	}

	template<typename... Args>
	static std::unique_ptr<properties_window_i> create (Args... args)
	{
		return std::make_unique<properties_window>(std::forward<Args>(args)...);
	}

	void set_selection_to_pgs()
	{
		const auto& objs = _selection->objects();

		if (objs.empty())
		{
			_pg1->clear();
			return;
		}

		stringstream ss;
		ss << "VLAN " << _project_window->selected_vlan_number() << " Specific Properties";
		auto pg_tree_title = ss.str();

		if (all_of (objs.begin(), objs.end(), [](object* o) { return o->is<Bridge>(); }))
		{
			_pg1->clear();
			_pg1->add_section ("Bridge Properties", objs.data(), objs.size());

			std::vector<object*> bridge_trees;
			for (object* o : objs)
			{
				auto b = static_cast<Bridge*>(o);
				auto tree_index = STP_GetTreeIndexFromVlanNumber(b->stp_bridge(), _project_window->selected_vlan_number());
				bridge_trees.push_back (b->trees().at(tree_index).get());
			}

			_pg1->add_section (pg_tree_title.c_str(), bridge_trees.data(), bridge_trees.size());
		}
		else if (all_of (objs.begin(), objs.end(), [](object* o) { return o->is<Port>(); }))
		{
			_pg1->clear();
			_pg1->add_section("Port Properties", objs.data(), objs.size());

			std::vector<object*> port_trees;
			for (object* o : objs)
			{
				auto p = static_cast<Port*>(o);
				auto tree_index = STP_GetTreeIndexFromVlanNumber(p->bridge()->stp_bridge(), _project_window->selected_vlan_number());
				port_trees.push_back (p->trees().at(tree_index).get());
			}

			_pg1->add_section (pg_tree_title.c_str(), port_trees.data(), port_trees.size());
		}
		else if (all_of (objs.begin(), objs.end(), [](object* o) { return o->is<Wire>(); }))
		{
			_pg1->clear();
			_pg1->add_section("Wire Properties", objs.data(), objs.size());
			_pg1->add_section(pg_tree_title.c_str(), nullptr, 0);
		}
		else
			assert(false); // not implemented

		move_pgs();
	}

	static void on_project_changed (void* callbackArg, IProject* project)
	{
		auto window = static_cast<properties_window*>(callbackArg);
		window->set_selection_to_pgs();
	}

	static void on_selection_changed (void* callbackArg, selection_i* selection)
	{
		auto window = static_cast<properties_window*>(callbackArg);
		window->set_selection_to_pgs();
	}

	static void on_selected_vlan_changed (void* callbackArg, IProjectWindow* pw, unsigned int selectedVlan)
	{
		auto window = static_cast<properties_window*>(callbackArg);
		window->set_selection_to_pgs();
	}

	static void on_pg12_preferred_height_changed (void* arg)
	{
		auto window = static_cast<properties_window*>(arg);
		window->move_pgs();
	}

	void move_pgs()
	{
		RECT rect = client_rect_pixels();
		BOOL bRes = ::MoveWindow (_pg1->hwnd(), rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, TRUE); assert(bRes);
	}

	optional<LRESULT> window_proc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) final
	{
		auto resultBaseClass = base::window_proc (hwnd, msg, wParam, lParam);

		if (msg == WM_SIZE)
		{
			if (_pg1 != nullptr)
				move_pgs();
			return 0;
		}

		return resultBaseClass;
	}
};

const properties_window_factory_t properties_window_factory = &properties_window::create;
