/*
 * Copyright (C) 2008-2009 Hans Baier <hansfbaier@googlemail.com>
 * Copyright (C) 2009-2011 Carl Hetherington <carl@carlh.net>
 * Copyright (C) 2009-2011 David Robillard <d@drobilla.net>
 * Copyright (C) 2009-2017 Paul Davis <paul@linuxaudiosystems.com>
 * Copyright (C) 2013-2015 Robin Gareus <robin@gareus.org>
 * Copyright (C) 2013 Michael Fisher <mfisher31@gmail.com>
 * Copyright (C) 2015-2016 Nick Mainsbridge <mainsbridge@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "pbd/compose.h"
#include "pbd/stacktrace.h"

#include "evoral/midi_events.h"

#include "ardour/async_midi_port.h"
#include "ardour/audioengine.h"
#include "ardour/debug.h"
#include "ardour/lmath.h"
#include "ardour/midi_buffer.h"
#include "ardour/midi_port.h"
#include "ardour/session.h"
#include "ardour/tempo.h"
#include "ardour/ticker.h"

using namespace ARDOUR;
using namespace PBD;

MidiClockTicker::MidiClockTicker (Session* s)
{
	_session = s;
	_midi_port = s->midi_clock_output_port ();
	reset ();
}

MidiClockTicker::~MidiClockTicker ()
{
}

void
MidiClockTicker::reset ()
{
	_rolling = false;
	_last_tick = 0;
	_beat_pos = 0;
	_clock_cnt = 0;
	_transport_pos = -1;
}

void
MidiClockTicker::tick (samplepos_t start_sample, samplepos_t end_sample, pframes_t n_samples)
{
	/* silence buffer */
	_midi_port->cycle_start (n_samples);

	double speed = (end_sample - start_sample) / (double)n_samples;

	if (!Config->get_send_midi_clock () /*|| !TransportMasterManager::instance().current()*/) {
		if (_rolling) {
			send_stop_event (0, n_samples);
		}
		reset ();
		goto out;
	}

	// TODO: cache
	_midi_port->get_connected_latency_range(_mclk_out_latency, true);

	if (speed == 0 && start_sample == 0 && end_sample == 0) {

#if 0 // TODO THINK, sleep first :)
		/* test if pre-roll is active, special-case
		 * "start at zero"
		 */

		samplecnt_t rlp = _session->remaining_latency_preroll ();

		if (rlp > 0 && rlp < _mclk_out_latency.max) {
			printf ("PRE_START !!!\n");
			pframes_t pos = _mclk_out_latency.max - rlp;

			send_start_event (pos, n_samples);
			send_midi_clock_event (pos, n_samples);

			/* XXX may need to send more than one clock */

			_last_tick = pos + one_ppqn_in_samples (0) - rlp;

			_rolling = true; /// IFF next cycle rolls >= 0;
			_beat_pos = 0;
			_clock_cnt = 1;
			_transport_pos = 0;

			speed = 1.0;
		}
#endif
	}

	if (speed != 1.0) {
		if (_rolling) {
			send_stop_event (0, n_samples);
		}
		reset ();
		goto out;
	}

	/* test for discontinuity */
	if (start_sample != _transport_pos) {
		if (_rolling) {
			send_stop_event (0, n_samples);
		}
		_rolling = false;
		_transport_pos = -1;
	}

	if (!_rolling) {
		if (_transport_pos < 0 || _last_tick < start_sample) {
			/* get the next downbeat */
			uint32_t beat_pos;
			samplepos_t clk_pos;

			_session->tempo_map().midi_clock_beat_at_of_after (start_sample + _mclk_out_latency.max, clk_pos, beat_pos);

			_beat_pos      = beat_pos;
			_last_tick     = clk_pos;
			_transport_pos = end_sample;
		}

		if (_last_tick >= start_sample && _last_tick < end_sample) {
			_rolling = true;
			_clock_cnt = 0;
			if (_beat_pos == 0 && _last_tick == 0 && start_sample == 0) {
				send_start_event (0, n_samples);
			} else {
				send_position_event (_beat_pos, 0, n_samples); // consider sending this early
				send_continue_event (_last_tick - start_sample, n_samples);
			}
		} else {
			goto out;
		}
	}

	assert (_rolling);

	while (_last_tick >= start_sample && _last_tick < end_sample) {
		send_midi_clock_event (_last_tick - start_sample, n_samples);
		if (++_clock_cnt == 6) {
			_clock_cnt = 0;
			++_beat_pos;
		}
		_last_tick += one_ppqn_in_samples (llrint (_last_tick));
	}

	_transport_pos = end_sample;

out:
	_midi_port->flush_buffers (n_samples);
	_midi_port->cycle_end (n_samples);
}

double
MidiClockTicker::one_ppqn_in_samples (samplepos_t transport_position) const
{
	const double samples_per_quarter_note = _session->tempo_map ().samples_per_quarter_note_at (transport_position, _session->nominal_sample_rate ());

	return samples_per_quarter_note / 24.0;
}

void
MidiClockTicker::send_midi_clock_event (pframes_t offset, pframes_t nframes)
{
	assert (_midi_port);

	static uint8_t msg = MIDI_CMD_COMMON_CLOCK;

	MidiBuffer& mb (_midi_port->get_midi_buffer (nframes));
	mb.push_back (offset, 1, &msg);

	DEBUG_TRACE (DEBUG::MidiClock, string_compose ("Tick with offset %1\n", offset));
}

void
MidiClockTicker::send_start_event (pframes_t offset, pframes_t nframes)
{
	assert (_midi_port);

	static uint8_t msg = { MIDI_CMD_COMMON_START };
	MidiBuffer&    mb (_midi_port->get_midi_buffer (nframes));
	mb.push_back (offset, 1, &msg);

	DEBUG_TRACE (DEBUG::MidiClock, string_compose ("Start %1\n", _last_tick));
}

void
MidiClockTicker::send_continue_event (pframes_t offset, pframes_t nframes)
{
	assert (_midi_port);

	static uint8_t msg = { MIDI_CMD_COMMON_CONTINUE };
	MidiBuffer&    mb (_midi_port->get_midi_buffer (nframes));
	mb.push_back (offset, 1, &msg);

	DEBUG_TRACE (DEBUG::MidiClock, string_compose ("Continue %1\n", _last_tick));
}

void
MidiClockTicker::send_stop_event (pframes_t offset, pframes_t nframes)
{
	assert (_midi_port);

	static uint8_t msg = MIDI_CMD_COMMON_STOP;
	MidiBuffer&    mb (_midi_port->get_midi_buffer (nframes));
	mb.push_back (offset, 1, &msg);

	DEBUG_TRACE (DEBUG::MidiClock, string_compose ("Stop %1\n", _last_tick));
}

void
MidiClockTicker::send_position_event (uint32_t midi_beats, pframes_t offset, pframes_t nframes)
{
	assert (_midi_port);

	/* can only use 14bits worth */
	if (midi_beats > 0x3fff) {
		return;
	}

	/* split midi beats into a 14bit value */
	MIDI::byte msg[3];
	msg[0] = MIDI_CMD_COMMON_SONG_POS;
	msg[1] = midi_beats & 0x007f;
	msg[2] = midi_beats >> 7;

	MidiBuffer& mb (_midi_port->get_midi_buffer (nframes));
	mb.push_back (offset, 3, &msg[0]);

	DEBUG_TRACE (DEBUG::MidiClock, string_compose ("Song Position Sent: %1 to %2 (events now %3, buf = %4)\n", midi_beats, _midi_port->name (), mb.size (), &mb));
}
