/*
** Copyright (C) 2004 Jesse Chappell <jesse@essej.net>
**  
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**  
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**  
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**  
*/
#include <iostream>

#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>

#include "engine.hpp"
#include "looper.hpp"
#include "control_osc.hpp"

using namespace SooperLooper;
using namespace std;
using namespace PBD;

#define MAX_EVENTS 1024


Engine::Engine ()
{
	_ok = false;
	_driver = 0;
	_osc = 0;
	_event_generator = 0;
	_event_queue = 0;
	_def_channel_cnt = 2;
	_def_loop_secs = 200;
	_tempo = 110.0f;
	_eighth_cycle = 16.0f;
	_sync_source = NoSync;
	_tempo_counter = 0;
	
	pthread_cond_init (&_event_cond, NULL);

}

bool Engine::initialize(AudioDriver * driver, int port, string pingurl)
{

	_driver = driver;
	_driver->set_engine(this);
	
	if (!_driver->initialize()) {
		cerr << "cannot connect to audio driver" << endl;
		return false;
	}
	

	_event_generator = new EventGenerator(_driver->get_samplerate());
	_event_queue = new RingBuffer<Event> (MAX_EVENTS);

	_nonrt_event_queue = new RingBuffer<EventNonRT *> (MAX_EVENTS);

	_internal_sync_buf = new float[driver->get_buffersize()];
	memset(_internal_sync_buf, 0, sizeof(float) * driver->get_buffersize());

	calculate_tempo_frames();
	
	_osc = new ControlOSC(this, port);

	if (!_osc->is_ok()) {
		return false;
	}

	_ok = true;

	return true;
}


void
Engine::cleanup()
{
	if (_osc) {
		delete _osc;
	}

	if (_event_queue) {
		delete _event_queue;
		_event_queue = 0;
	}
	if (_event_generator) {
		delete _event_generator;
		_event_generator = 0;
	}

	_ok = false;
	
}

Engine::~Engine ()
{
	cleanup ();
}

void Engine::quit()
{
	
	_ok = false;

	LockMonitor mon(_event_loop_lock, __LINE__, __FILE__);
	pthread_cond_signal (&_event_cond);
}


bool
Engine::add_loop (unsigned int chans)
{
	int n;
	
	{
		LockMonitor lm (_instance_lock, __LINE__, __FILE__);
		n = _instances.size();

		Looper * instance;
		
		instance = new Looper (_driver, (unsigned int) n, chans);
		
		if (!(*instance)()) {
			cerr << "can't create a new loop!\n";
			delete instance;
			return false;
		}
		
		_instances.push_back (instance);

		update_sync_source();
	}
	
	LoopAdded (n); // emit
	
	return true;
}


bool
Engine::remove_loop (unsigned int index)
{
	LockMonitor lm (_instance_lock, __LINE__, __FILE__);

	if (index < _instances.size()) {

		Instances::iterator iter = _instances.begin();
		iter += index;
		
		Looper * loop = (*iter);
		_instances.erase (iter);

		delete loop;
		LoopRemoved(); // emit

		update_sync_source();
		
		return true;
	}

	return false;
}


std::string
Engine::get_osc_url ()
{
	if (_osc && _osc->is_ok()) {
		return _osc->get_server_url();
	}

	return "";
}

int
Engine::get_osc_port ()
{
	if (_osc && _osc->is_ok()) {
		return _osc->get_server_port();
	}

	return 0;
}

int
Engine::process (nframes_t nframes)
{
	TentativeLockMonitor lm (_instance_lock, __LINE__, __FILE__);

	if (!lm.locked()) {
		// todo pass silence
		//cerr << "already locked!" << endl;

		//_driver->process_silence (nframes);
		
		return 0;
	}

	// process events
	//cerr << "process"  << endl;

	Event * evt;
	RingBuffer<Event>::rw_vector vec;
		
	// get available events
	_event_queue->get_read_vector (&vec);
		
	// update event generator
	_event_generator->updateFragmentTime (nframes);

	// update internal sync
	generate_sync (nframes);
	

	nframes_t usedframes = 0;
	nframes_t doframes;
	size_t num = vec.len[0];
	size_t n = 0;
	size_t vecn = 0;
	nframes_t fragpos;
		
	if (num > 0) {
		
		while (n < num)
		{ 
			evt = vec.buf[vecn] + n;
			fragpos = (nframes_t) evt->FragmentPos();

			++n;
			// to avoid code copying
			if (n == num) {
				if (vecn == 0) {
					++vecn;
					n = 0;
					num = vec.len[1];
				}
			}
				
			if (fragpos < usedframes || fragpos >= nframes) {
				// bad fragment pos
#ifdef DEBUG
				cerr << "BAD FRAGMENT POS: " << fragpos << endl;
#endif
				continue;
			}
				
			doframes = fragpos - usedframes;
			int m = 0;
			for (Instances::iterator i = _instances.begin(); i != _instances.end(); ++i, ++m)
			{
				// run for the time before this event
				(*i)->run (usedframes, doframes);
					
				// process event
				if (evt->Instance == -1 || evt->Instance == m) {
					(*i)->do_event (evt);
				}
			}
				
			usedframes += doframes;
		}

		// advance events
		_event_queue->increment_read_ptr (vec.len[0] + vec.len[1]);
			
		// run the rest of the frames
		for (Instances::iterator i = _instances.begin(); i != _instances.end(); ++i) {
			(*i)->run (usedframes, nframes - usedframes);
		}

	}
	else {
		// no events
		for (Instances::iterator i = _instances.begin(); i != _instances.end(); ++i) {
			(*i)->run (0, nframes);
		}

	}

	
	return 0;
}


bool
Engine::push_command_event (Event::type_t type, Event::command_t cmd, int8_t instance)
{
	// todo support more than one simulataneous pusher safely
	RingBuffer<Event>::rw_vector vec;

	_event_queue->get_write_vector (&vec);

	if (vec.len[0] == 0) {
#ifdef DEBUG
		cerr << "event queue full, dropping event" << endl;
#endif
		return false;
	}
	
	Event * evt = vec.buf[0];
	*evt = get_event_generator().createEvent();

	evt->Type = type;
	evt->Command = cmd;
	evt->Instance = instance;

	_event_queue->increment_write_ptr (1);

	return true;
}


bool
Engine::push_control_event (Event::type_t type, Event::control_t ctrl, float val, int8_t instance)
{
	// todo support more than one simulataneous pusher safely
	
	RingBuffer<Event>::rw_vector vec;

	_event_queue->get_write_vector (&vec);

	if (vec.len[0] == 0) {
#ifdef DEBUG
		cerr << "event queue full, dropping event" << endl;
#endif
		return false;
	}
	
	Event * evt = vec.buf[0];
	*evt = get_event_generator().createEvent();

	evt->Type = type;
	evt->Control = ctrl;
	evt->Value = val;
	evt->Instance = instance;

	_event_queue->increment_write_ptr (1);

	
	return true;
}


float
Engine::get_control_value (Event::control_t ctrl, int8_t instance)
{
	// this should *really* be mutexed
	// it is a race waiting to happen

	// not really anymore, this is only called from the nonrt work thread
	// that does the allocating of instances
	
	if (instance >= 0 && instance < (int) _instances.size()) {

		return _instances[instance]->get_control_value (ctrl);
	}

	return 0.0f;
}


bool
Engine::push_nonrt_event (EventNonRT * event)
{

	_nonrt_event_queue->write(&event, 1);
	
	LockMonitor mon(_event_loop_lock,  __LINE__, __FILE__);
	pthread_cond_signal (&_event_cond);

	return true;
}


void
Engine::mainloop()
{
	struct timespec timeout;
	struct timeval now;

	EventNonRT * event;
	
	// non-rt event processing loop

	while (is_ok())
	{
		// pull off all events from nonrt ringbuffer
		while (is_ok() && _nonrt_event_queue->read(&event, 1) == 1)
		{
			process_nonrt_event (event);
			delete event;
		}
		

		if (!is_ok()) break;
		
		// sleep on condition
		{
			LockMonitor mon(_event_loop_lock, __LINE__, __FILE__);
			gettimeofday(&now, NULL);
			timeout.tv_sec = now.tv_sec + 5;
			timeout.tv_nsec = now.tv_usec * 1000;
			pthread_cond_timedwait (&_event_cond, _event_loop_lock.mutex(), &timeout);
		}
	}

}

bool
Engine::process_nonrt_event (EventNonRT * event)
{
	ConfigUpdateEvent * cu_event;
	GetParamEvent *     gp_event;
	ConfigLoopEvent *   cl_event;
	PingEvent *         ping_event;
	RegisterConfigEvent * rc_event;
	LoopFileEvent      * lf_event;
	GlobalGetEvent     * gg_event;
	GlobalSetEvent     * gs_event;

	if ((gp_event = dynamic_cast<GetParamEvent*> (event)) != 0)
	{
		gp_event->ret_value = get_control_value (gp_event->control, gp_event->instance);
		_osc->finish_get_event (*gp_event);
	}
	else if ((gg_event = dynamic_cast<GlobalGetEvent*> (event)) != 0)
	{
		if (gg_event->param == "sync_source") {
			gg_event->ret_value = (float) _sync_source;
		}
		else if (gg_event->param == "tempo") {
			gg_event->ret_value = _tempo;
		}
		else if (gg_event->param == "eighth_per_cycle") {
			gg_event->ret_value = _eighth_cycle;
		}

		_osc->finish_global_get_event (*gg_event);
	}
	else if ((gs_event = dynamic_cast<GlobalSetEvent*> (event)) != 0)
	{
		if (gs_event->param == "sync_source") {
			if ((int) gs_event->value > (int) FIRST_SYNC_SOURCE
			    && gs_event->value <= _instances.size())
			{
				_sync_source = (SyncSourceType) (int) gs_event->value;
				update_sync_source();
				calculate_tempo_frames();
			}
		}
		else if (gs_event->param == "tempo") {
			if (gs_event->value > 0.0f) {
				_tempo = gs_event->value;
				_tempo_counter = 0;
				calculate_tempo_frames();
			}
		}
		else if (gs_event->param == "eighth_per_cycle") {
			if (gs_event->value > 0.0f) {
				_eighth_cycle = gs_event->value;
				calculate_tempo_frames();
			}
		}
	}
	else if ((cu_event = dynamic_cast<ConfigUpdateEvent*> (event)) != 0)
	{
		_osc->finish_update_event (*cu_event);
	}
	else if ((cl_event = dynamic_cast<ConfigLoopEvent*> (event)) != 0)
	{
		if (cl_event->type == ConfigLoopEvent::Add) {
			// todo: use secs
			if (cl_event->channels == 0) {
				cl_event->channels = _def_channel_cnt;
			}
					
			add_loop (cl_event->channels);
		}
		else if (cl_event->type == ConfigLoopEvent::Remove)
		{
			if (cl_event->index == -1) {
				cl_event->index = _instances.size() - 1;
			}
			remove_loop (cl_event->index);
			_osc->finish_loop_config_event (*cl_event);
		}
	}
	else if ((ping_event = dynamic_cast<PingEvent*> (event)) != 0)
	{
		_osc->send_pingack(ping_event->ret_url, ping_event->ret_path);
	}
	else if ((rc_event = dynamic_cast<RegisterConfigEvent*> (event)) != 0)
	{
		_osc->finish_register_event (*rc_event);
	}
	else if ((lf_event = dynamic_cast<LoopFileEvent*> (event)) != 0)
	{
		for (unsigned int n=0; n < _instances.size(); ++n) {
			if (lf_event->instance == -1 || lf_event->instance == (int)n) {
				if (lf_event->type == LoopFileEvent::Load) {
					_instances[n]->load_loop (lf_event->filename);
				}
				else {
					_instances[n]->save_loop (lf_event->filename);
				}
			}
		}
	}
	
	return true;
}


void Engine::update_sync_source ()
{
	float * sync_buf = _internal_sync_buf;

	// if sync_source > 0, then get the source from instance
	if (_sync_source == JackSync) {
		sync_buf = _internal_sync_buf;
	}
	else if (_sync_source == MidiClockSync) {
		sync_buf = _internal_sync_buf;
	}
	else if (_sync_source == InternalTempoSync) {
		sync_buf = _internal_sync_buf;
	}
	else if (_sync_source == BrotherSync) {
		sync_buf = _internal_sync_buf;
	}
	else if (_sync_source > 0 && (int)_sync_source <= (int) _instances.size()) {
		sync_buf = _instances[(int)_sync_source - 1]->get_sync_out_buf();
	}
	
	
	for (Instances::iterator i = _instances.begin(); i != _instances.end(); ++i)
	{
		(*i)->use_sync_buf (sync_buf);
	}
}


void
Engine::calculate_tempo_frames ()
{
	// TODO: use floats!
	float quantize_value = (float) QUANT_8TH;
		
	if (!_instances.empty()) {
		quantize_value = _instances[0]->get_control_value (Event::Quantize);
	}
	
	if (_sync_source == InternalTempoSync)
	{
		if (quantize_value == QUANT_8TH) {
			// calculate number of samples per eighth-note (assuming 2 8ths per beat)
			// samples / 8th = samplerate * (1 / tempo) * 60/2; 
			_tempo_frames = (nframes_t) lrint(_driver->get_samplerate() * (1/_tempo) * 30.0);
		}
		else if (quantize_value == QUANT_CYCLE) {
			// calculate number of samples per cycle given the current eighths per cycle
			// samples / 8th = samplerate * (1 / tempo) * 60/2; 
			// samples / cycle = samples / 8th  *  eighth_per_cycle
			_tempo_frames = (nframes_t) (lrint(_driver->get_samplerate() * (1/_tempo) * 30.0) * _eighth_cycle);
		}
		else {
			_tempo_frames = 0; // ???
		}

		cerr << "tempo frames is " << _tempo_frames << endl;
	}

}

void
Engine::generate_sync (nframes_t nframes)
{
	if (_sync_source == InternalTempoSync && _tempo_frames != 0) {
		nframes_t npos = 0;
		nframes_t curr = _tempo_counter;
		
		while (npos < nframes) {
			
			while (curr < _tempo_frames && npos < nframes) {
				_internal_sync_buf[npos++] = 0.0f;
				curr++;
			}

			if (npos < nframes) {
				//cerr << "tempo hit" << endl;
				_internal_sync_buf[npos++] = 1.0f;
				// reset curr counter
				curr = 1;
			}
		}

		_tempo_counter = curr;
		//cerr << "tempo counter is now: " << _tempo_counter << endl;
	}
	else {
		memset (_internal_sync_buf, 0, nframes * sizeof(float));
	}
}
