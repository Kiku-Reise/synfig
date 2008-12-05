/* === S Y N F I G ========================================================= */
/*!	\file layer.cpp
**	\brief Layer class implementation
**
**	$Id$
**
**	\legal
**	Copyright (c) 2002-2005 Robert B. Quattlebaum Jr., Adrian Bentley
**	Copyright (c) 2007, 2008 Chris Moore
**
**	This package is free software; you can redistribute it and/or
**	modify it under the terms of the GNU General Public License as
**	published by the Free Software Foundation; either version 2 of
**	the License, or (at your option) any later version.
**
**	This package is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
**	General Public License for more details.
**	\endlegal
*/
/* ========================================================================= */

/* === H E A D E R S ======================================================= */

#define SYNFIG_NO_ANGLE

#ifdef USING_PCH
#	include "pch.h"
#else
#ifdef HAVE_CONFIG_H
#	include <config.h>
#endif

#include "canvas.h"
#include "layer.h"
#include "render.h"
#include "value.h"
#include "layer_bitmap.h"
#include "layer_mime.h"
#include "context.h"
#include "paramdesc.h"

#include "layer_solidcolor.h"
#include "layer_polygon.h"
#include "layer_pastecanvas.h"
#include "layer_motionblur.h"
#include "layer_duplicate.h"
#include "layer_skeleton.h"

#include "valuenode_const.h"

#include "transform.h"
#include "rect.h"
#include "guid.h"

#include <sigc++/adaptors/bind.h>
#endif

/* === U S I N G =========================================================== */

using namespace etl;
using namespace std;
using namespace synfig;

/* === G L O B A L S ======================================================= */

static Layer::Book* _layer_book;

struct _LayerCounter
{
	static int counter;
	~_LayerCounter()
	{
		if(counter)
			synfig::error("%d layers not yet deleted!",counter);
	}
} _layer_counter;

int _LayerCounter::counter(0);

/* === P R O C E D U R E S ================================================= */

Layer::Book&
Layer::book()
{
	return *_layer_book;
}

void
Layer::register_in_book(const BookEntry &entry)
{
	book()[entry.name]=entry;
}

bool
Layer::subsys_init()
{
	_layer_book=new Book();

#define INCLUDE_LAYER(class)									\
	synfig::Layer::book() [synfig::String(class::name__)] =		\
		BookEntry(class::create,								\
				  class::name__,								\
				  dgettext("synfig", class::local_name__),		\
				  class::category__,							\
				  class::cvs_id__,								\
				  class::version__)

#define LAYER_ALIAS(class,alias)								\
	synfig::Layer::book()[synfig::String(alias)] =				\
		BookEntry(class::create,								\
				  alias,										\
				  alias,										\
				  CATEGORY_DO_NOT_USE,							\
				  class::cvs_id__,								\
				  class::version__)

	INCLUDE_LAYER(Layer_SolidColor);	LAYER_ALIAS(Layer_SolidColor,	"solid_color");
	INCLUDE_LAYER(Layer_PasteCanvas);	LAYER_ALIAS(Layer_PasteCanvas,	"paste_canvas");
	INCLUDE_LAYER(Layer_Polygon);		LAYER_ALIAS(Layer_Polygon,		"Polygon");
	INCLUDE_LAYER(Layer_MotionBlur);	LAYER_ALIAS(Layer_MotionBlur,	"motion_blur");
	INCLUDE_LAYER(Layer_Duplicate);
	INCLUDE_LAYER(Layer_Skeleton);

#undef INCLUDE_LAYER

	return true;
}

bool
Layer::subsys_stop()
{
	delete _layer_book;
	return true;
}

/* === M E T H O D S ======================================================= */

Layer::Layer():
	active_(true),
	z_depth_(0.0f),
	dirty_time_(Time::end())
{
	_LayerCounter::counter++;
}

Layer::LooseHandle
synfig::Layer::create(const String &name)
{
	if(!book().count(name))
	{
		return Layer::LooseHandle(new Layer_Mime(name));
	}

	Layer* layer(book()[name].factory());
	return Layer::LooseHandle(layer);
}

synfig::Layer::~Layer()
{
	_LayerCounter::counter--;
	while(!dynamic_param_list_.empty())
	{
		remove_child(dynamic_param_list_.begin()->second.get());
		dynamic_param_list_.erase(dynamic_param_list_.begin());
	}

	remove_from_all_groups();

	parent_death_connect_.disconnect();
	begin_delete();
}

void
synfig::Layer::set_canvas(etl::loose_handle<Canvas> x)
{
	if(canvas_!=x)
	{
		parent_death_connect_.disconnect();
		canvas_=x;
		if(x)
		{
			parent_death_connect_=x->signal_deleted().connect(
				sigc::bind(
					sigc::mem_fun(
						*this,
						&Layer::set_canvas
					),
					etl::loose_handle<synfig::Canvas>(0)
				)
			);
		}
		on_canvas_set();
	}
}

void
synfig::Layer::on_canvas_set()
{
}

etl::loose_handle<synfig::Canvas>
synfig::Layer::get_canvas()const
{
	return canvas_;
}

int
Layer::get_depth()const
{
	if(!get_canvas())
		return -1;
	return get_canvas()->get_depth(const_cast<synfig::Layer*>(this));
}

void
Layer::set_active(bool x)
{
	if(active_!=x)
	{
		active_=x;

		Node::on_changed();
		signal_status_changed_();
	}
}

void
Layer::set_description(const String& x)
{
	if(description_!=x)
	{
		description_=x;
		signal_description_changed_();
	}
}

bool
Layer::connect_dynamic_param(const String& param, etl::loose_handle<ValueNode> value_node)
{
	ValueNode::Handle previous(dynamic_param_list_[param]);

	if(previous==value_node)
		return true;

	dynamic_param_list_[param]=ValueNode::Handle(value_node);

	if(previous)
		remove_child(previous.get());

	add_child(value_node.get());

	if(!value_node->is_exported() && get_canvas())
	{
		value_node->set_parent_canvas(get_canvas());
	}

	changed();
	return true;
}

bool
Layer::disconnect_dynamic_param(const String& param)
{
	ValueNode::Handle previous(dynamic_param_list_[param]);

	if(previous)
	{
		dynamic_param_list_.erase(param);

		// fix 2353284: if two parameters in the same layer are
		// connected to the same valuenode and we disconnect one of
		// them, the parent-child relationship for the remaining
		// connection was being deleted.  now we search the parameter
		// list to see if another parameter uses the same valuenode
		DynamicParamList::const_iterator iter;
		for (iter = dynamic_param_list().begin(); iter != dynamic_param_list().end(); iter++)
			if (iter->second == previous)
				break;
		if (iter == dynamic_param_list().end())
			remove_child(previous.get());

		changed();
	}
	return true;
}

void
Layer::on_changed()
{
	if (getenv("SYNFIG_DEBUG_ON_CHANGED"))
		printf("%s:%d Layer::on_changed()\n", __FILE__, __LINE__);

	dirty_time_=Time::end();
	Node::on_changed();
}

bool
Layer::set_param(const String &param, const ValueBase &value)
{
	if(param=="z_depth" && value.same_type_as(z_depth_))
	{
		z_depth_=value.get(z_depth_);
		return true;
	}
	return false;
}

etl::handle<Transform>
Layer::get_transform()const
{
	return 0;
}

float
Layer::get_z_depth(const synfig::Time& t)const
{
	if(!dynamic_param_list().count("z_depth"))
		return z_depth_;
	return (*dynamic_param_list().find("z_depth")->second)(t).get(Real());
}

Layer::Handle
Layer::simple_clone()const
{
	if(!book().count(get_name())) return 0;
	Handle ret = create(get_name()).get();
	ret->group_=group_;
	//ret->set_canvas(get_canvas());
	ret->set_description(get_description());
	ret->set_active(active());
	ret->set_param_list(get_param_list());
	for(DynamicParamList::const_iterator iter=dynamic_param_list().begin();iter!=dynamic_param_list().end();++iter)
		ret->connect_dynamic_param(iter->first, iter->second);
	return ret;
}

Layer::Handle
Layer::clone(const GUID& deriv_guid) const
{
	if(!book().count(get_name())) return 0;

	//Layer *ret = book()[get_name()].factory();//create(get_name()).get();
	Handle ret = create(get_name()).get();

	ret->group_=group_;
	//ret->set_canvas(get_canvas());
	ret->set_description(get_description());
	ret->set_active(active());
	ret->set_guid(get_guid()^deriv_guid);

	//ret->set_param_list(get_param_list());
	// Process the parameter list so that
	// we can duplicate any inline canvases
	ParamList param_list(get_param_list());
	for(ParamList::const_iterator iter(param_list.begin()); iter != param_list.end(); ++iter)
	{
		if(dynamic_param_list().count(iter->first)==0 && iter->second.get_type()==ValueBase::TYPE_CANVAS)
		{
			// This parameter is a canvas.  We need a close look.
			Canvas::Handle canvas(iter->second.get(Canvas::Handle()));
			if(canvas && canvas->is_inline())
			{
				// This parameter is an inline canvas! we need to clone it
				// before we set it as a parameter.
				Canvas::Handle new_canvas(canvas->clone(deriv_guid));
				ValueBase value(new_canvas);
				ret->set_param(iter->first, value);
				continue;
			}
		}

		// This is a normal parameter,go ahead and set it.
		ret->set_param(iter->first, iter->second);
	}

	// Duplicate the dynamic paramlist, but only the exported data nodes
	DynamicParamList::const_iterator iter;
	for(iter=dynamic_param_list().begin();iter!=dynamic_param_list().end();++iter)
	{
		// Make sure we clone inline canvases
		if(iter->second->get_type()==ValueBase::TYPE_CANVAS)
		{
			Canvas::Handle canvas((*iter->second)(0).get(Canvas::Handle()));
			if(canvas->is_inline())
			{
				Canvas::Handle new_canvas(canvas->clone(deriv_guid));
				ValueBase value(new_canvas);
				ret->connect_dynamic_param(iter->first,ValueNode_Const::create(value));
				continue;
			}
		}

		if(iter->second->is_exported())
			ret->connect_dynamic_param(iter->first,iter->second);
		else
			ret->connect_dynamic_param(iter->first,iter->second->clone(deriv_guid));
	}

	//ret->set_canvas(0);

	return ret;
}

bool
Layer::reads_context() const
{
	return false;
}

Rect
Layer::get_full_bounding_rect(Context context)const
{
	if(active())
		return context.get_full_bounding_rect()|get_bounding_rect();
	return context.get_full_bounding_rect();
}

Rect
Layer::get_bounding_rect()const
{
	return Rect::full_plane();
}

bool
Layer::set_param_list(const ParamList &list)
{
	bool ret=true;
	if(!list.size())
		return false;
	ParamList::const_iterator iter(list.begin());
	for(;iter!=list.end();++iter)
	{
		if(!set_param(iter->first, iter->second))ret=false;
	}
	return ret;
}

Layer::ParamList
Layer::get_param_list()const
{
	ParamList ret;

	Vocab vocab(get_param_vocab());

	Vocab::const_iterator iter=vocab.begin();
	for(;iter!=vocab.end();++iter)
	{
		ret[iter->get_name()]=get_param(iter->get_name());
	}
	return ret;
}

ValueBase
Layer::get_param(const String & param)const
{
	if(param=="z_depth")
		return get_z_depth();

	return ValueBase();
}

String
Layer::get_version()const
{
	return get_param("version__").get(String());
}

bool
Layer::set_version(const String &/*ver*/)
{
	return false;
}

void
Layer::reset_version()
{
}


void
Layer::set_time(Context context, Time time)const
{
	context.set_time(time);
	dirty_time_=time;
}

void
Layer::set_time(Context context, Time time, const Point &pos)const
{
	context.set_time(time,pos);
	dirty_time_=time;
}

Color
Layer::get_color(Context context, const Point &pos)const
{
	return context.get_color(pos);
}

synfig::Layer::Handle
Layer::hit_check(synfig::Context context, const synfig::Point &pos)const
{
	return context.hit_check(pos);
}

/* 	The default accelerated renderer
**	is anything but accelerated...
*/
bool
Layer::accelerated_render(Context context,Surface *surface,int /*quality*/, const RendDesc &renddesc, ProgressCallback *cb)  const
{
	handle<Target> target=surface_target(surface);
	if(!target)
	{
		if(cb)cb->error(_("Unable to create surface target"));
		return false;
	}
	RendDesc desc=renddesc;
	target->set_rend_desc(&desc);

	// When we render, we want to
	// make sure that we are rendered too...
	// Since the context iterator is for
	// the layer after us, we need to back up.
	// This could be considered a hack, as
	// it is a possibility that we are indeed
	// not the previous layer.
	--context;

	return render(context,target,desc,cb);
	//return render_threaded(context,target,desc,cb,2);
}

String
Layer::get_name()const
{
	return get_param("name__").get(String());
}

String
Layer::get_local_name()const
{
	return get_param("local_name__").get(String());
}


Layer::Vocab
Layer::get_param_vocab()const
{
	Layer::Vocab ret;

	ret.push_back(ParamDesc(z_depth_,"z_depth")
		.set_local_name(_("Z Depth"))
		.set_animation_only(true)
	);

	return ret;
}

void
Layer::get_times_vfunc(Node::time_set &set) const
{
	DynamicParamList::const_iterator 	i = dynamic_param_list_.begin(),
										end = dynamic_param_list_.end();

	for(; i != end; ++i)
	{
		const Node::time_set &tset = i->second->get_times();
		set.insert(tset.begin(),tset.end());
	}
}


void
Layer::add_to_group(const String&x)
{
	if(x==group_)
		return;
	if(!group_.empty())
		remove_from_all_groups();
	group_=x;
	signal_added_to_group()(group_);
}

void
Layer::remove_from_group(const String&x)
{
	if(group_==x)
		remove_from_all_groups();
}

void
Layer::remove_from_all_groups()
{
	if(group_.empty())
		return;
	signal_removed_from_group()(group_);
	group_.clear();
}

String
Layer::get_group()const
{
	return group_;
}

const String
Layer::get_param_local_name(const String &param_name)const
{
	ParamVocab vocab = get_param_vocab();
	// loop to find the parameter in the parameter vocab - this gives us its local name
	for (ParamVocab::iterator iter = vocab.begin(); iter != vocab.end(); iter++)
		if (iter->get_name() == param_name)
			return iter->get_local_name();
	return String();
}
