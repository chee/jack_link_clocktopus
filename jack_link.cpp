// jack_link.cpp
//
/****************************************************************************
   Copyright (C) 2017-2021, rncbc aka Rui Nuno Capela. All rights reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

*****************************************************************************/

#include "jack_link.hpp"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <iostream>
#include <pigpiod_if2.h>
#include <sstream>
#include <string>

jack_link::jack_link(void)
    : m_link(120.0), m_client(nullptr), m_srate(44100.0), m_timebase(0),
      m_npeers(0), m_tempo(120.0), m_tempo_req(0.0), m_quantum(4.0),
      m_playing(false), m_playing_req(false), m_running(false),
      m_thread(nullptr) {
  m_link.setNumPeersCallback(
      [this](const std::size_t npeers) { peers_callback(npeers); });
  m_link.setTempoCallback(
      [this](const double tempo) { tempo_callback(tempo); });
  m_link.setStartStopCallback(
      [this](const bool playing) { playing_callback(playing); });

  m_link.enableStartStopSync(true);

  initialize();
}

jack_link::~jack_link(void) { terminate(); }

const char *jack_link::name(void) { return JACK_LINK_NAME; }

const char *jack_link::version(void) {
  return JACK_LINK_VERSION " (Link v" ABLETON_LINK_VERSION ")";
}

bool jack_link::active(void) const { return (m_client != nullptr); }

std::size_t jack_link::npeers(void) const { return m_npeers; }

double jack_link::srate(void) const { return m_srate; }

double jack_link::quantum(void) const { return m_quantum; }

void jack_link::tempo(double tempo) {
  if (m_npeers > 0) {
    auto session_state = m_link.captureAppSessionState();
    const auto host_time = m_link.clock().micros();
    session_state.setTempo(tempo, host_time);
    m_link.commitAppSessionState(session_state);
  } else {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tempo_req = tempo;
    timebase_reset();
    m_cond.notify_one();
  }
}

double jack_link::tempo(void) const { return m_tempo; }

void jack_link::playing(bool playing) {
  if (m_npeers > 0) {
    auto session_state = m_link.captureAppSessionState();
    const auto host_time = m_link.clock().micros();
    session_state.setIsPlaying(playing, host_time);
    m_link.commitAppSessionState(session_state);
  } else {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_playing_req = true;
    m_playing = playing;
    transport_reset();
    m_cond.notify_one();
  }
}

bool jack_link::playing(void) const { return m_playing; }

int jack_link::process_callback(jack_nframes_t /*nframes*/,
                                void * /*user_data*/) {
  return 0;
}

int jack_link::sync_callback(jack_transport_state_t state,
                             jack_position_t *position, void *user_data) {
  jack_link *pJackLink = static_cast<jack_link *>(user_data);
  return pJackLink->sync_callback(state, position);
}

int jack_link::sync_callback(jack_transport_state_t state,
                             jack_position_t *pos) {
  if (state == JackTransportStarting && m_playing && !m_playing_req) {
    // Sync to current JACK transport frame-beat quantum...
    auto session_state = m_link.captureAudioSessionState();
    const auto host_time = m_link.clock().micros();
    const double beat = position_beat(pos);
    session_state.forceBeatAtTime(beat, host_time, m_quantum);
    m_link.commitAudioSessionState(session_state);
  }

  return 1;
}

void jack_link::timebase_callback(jack_transport_state_t state,
                                  jack_nframes_t nframes, jack_position_t *pos,
                                  int new_pos, void *user_data) {
  jack_link *pJackLink = static_cast<jack_link *>(user_data);
  pJackLink->timebase_callback(state, nframes, pos, new_pos);
}

void jack_link::timebase_callback(jack_transport_state_t /*state*/,
                                  jack_nframes_t /*nframes*/,
                                  jack_position_t *pos, int new_pos) {
  if (m_tempo_req > 0.0 && m_mutex.try_lock()) {
    m_tempo = m_tempo_req;
    m_tempo_req = 0.0;
    m_mutex.unlock();
  }

  const auto frame_time = std::chrono::microseconds(
      std::llround(1.0e6 * pos->frame / pos->frame_rate));

  const double beats_per_minute = m_tempo;
  const double beats_per_bar = std::max(m_quantum, 1.0);

  const double beats = beats_per_minute * frame_time.count() / 60.0e6;
  const double bar = std::floor(beats / beats_per_bar);
  const double beat = beats - bar * beats_per_bar;

  const bool valid = (pos->valid & JackPositionBBT);
  const double ticks_per_beat = (valid ? pos->ticks_per_beat : 960.0);
  const float beat_type = (valid ? pos->beat_type : 4.0f);

  pos->valid = JackPositionBBT;
  pos->bar = int32_t(bar) + 1;
  pos->beat = int32_t(beat) + 1;
  pos->tick = int32_t(ticks_per_beat * (beat - std::floor(beat)));
  pos->beats_per_bar = float(beats_per_bar);
  pos->ticks_per_beat = ticks_per_beat;
  pos->beats_per_minute = beats_per_minute;
  pos->beat_type = beat_type;

  if (new_pos)
    ++m_timebase;
}

void jack_link::on_shutdown(void *user_data) {
  jack_link *pJackLink = static_cast<jack_link *>(user_data);
  pJackLink->on_shutdown();
}

void jack_link::on_shutdown(void) {
  std::cerr << "jack_link::on_shutdown()" << std::endl;
  m_client = nullptr;

  terminate();

  ::fclose(stdin);
  std::cerr << std::endl;
}

void jack_link::peers_callback(const std::size_t npeers) {
  std::lock_guard<std::mutex> lock(m_mutex);
  std::cerr << "jack_link::peers_callback(" << npeers << ")" << std::endl;
  m_npeers = npeers;
  timebase_reset();
  m_cond.notify_one();
}

void jack_link::tempo_callback(const double tempo) {
  std::lock_guard<std::mutex> lock(m_mutex);
  std::cerr << "jack_link::tempo_callback(" << tempo << ")" << std::endl;
  m_tempo_req = tempo;
  timebase_reset();
  m_cond.notify_one();
}

void jack_link::playing_callback(const bool playing) {
  if (m_playing_req && m_mutex.try_lock()) {
    m_playing_req = false;
    m_mutex.unlock();
    return;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  std::cerr << "jack_link::playing_callback(" << playing << ")" << std::endl;
  m_playing_req = true;
  m_playing = playing;
  transport_reset();
  m_cond.notify_one();
}

void jack_link::initialize(void) {
  m_thread = new std::thread([this] { worker_start(); });
  //	m_thread->detach();

  jack_status_t status = JackFailure;
  m_client = ::jack_client_open(jack_link::name(), JackNullOption, &status);
  if (m_client == nullptr) {
    std::stringstream ss;
    ss << jack_link::name() << ':' << ' ';
    const std::string &s = ss.str();
    std::cerr << s << "Could not initialize JACK client." << std::endl;
    if (status & JackFailure)
      std::cerr << s << "Overall operation failed." << std::endl;
    if (status & JackInvalidOption)
      std::cerr << s << "Invalid or unsupported option." << std::endl;
    if (status & JackNameNotUnique)
      std::cerr << s << "Client name not unique." << std::endl;
    if (status & JackServerStarted)
      std::cerr << s << "Server is started." << std::endl;
    if (status & JackServerFailed)
      std::cerr << s << "Unable to connect to server." << std::endl;
    if (status & JackServerError)
      std::cerr << s << "Server communication error." << std::endl;
    if (status & JackNoSuchClient)
      std::cerr << s << "Client does not exist." << std::endl;
    if (status & JackLoadFailure)
      std::cerr << s << "Unable to load internal client." << std::endl;
    if (status & JackInitFailure)
      std::cerr << s << "Unable to initialize client." << std::endl;
    if (status & JackShmFailure)
      std::cerr << s << "Unable to access shared memory." << std::endl;
    if (status & JackVersionError)
      std::cerr << s << "Client protocol version mismatch." << std::endl;
    std::cerr << std::endl;
    //	std::terminate();
    return;
  };

  m_srate = double(::jack_get_sample_rate(m_client));

  ::jack_set_process_callback(m_client, process_callback, this);
  ::jack_set_sync_callback(m_client, sync_callback, this);
  ::jack_on_shutdown(m_client, on_shutdown, this);

  ::jack_activate(m_client);

  m_link.enable(true);

  timebase_reset();
}

void jack_link::terminate(void) {
  worker_stop();

  if (m_thread) {
    m_thread->join();
    delete m_thread;
    m_thread = nullptr;
  }

  m_link.enable(false);

  if (m_client) {
    ::jack_deactivate(m_client);
    ::jack_client_close(m_client);
    m_client = nullptr;
  }
}

void jack_link::timebase_reset(void) {
  if (m_client == nullptr)
    return;

  if (m_timebase > 0) {
    ::jack_release_timebase(m_client);
    m_timebase = 0;
  }

  ::jack_set_timebase_callback(m_client, 0, jack_link::timebase_callback, this);
}

void jack_link::transport_reset(void) {
  if (m_client == nullptr)
    return;

  if (m_playing_req && m_playing && m_npeers > 0) {
    jack_position_t pos;
    const jack_transport_state_t state = ::jack_transport_query(m_client, &pos);
    if (state == JackTransportStopped) {
      // Sync to current JACK transport frame-beat quantum...
      auto session_state = m_link.captureAppSessionState();
      const auto host_time = m_link.clock().micros();
      const double beat = position_beat(&pos);
      session_state.forceBeatAtTime(beat, host_time, m_quantum);
      m_link.commitAppSessionState(session_state);
    }
  }

  // Start/stop playing on JACK...
  if (m_playing)
    ::jack_transport_start(m_client);
  else
    ::jack_transport_stop(m_client);
}

double jack_link::position_beat(jack_position_t *pos) const {
  if (pos->valid & JackPositionBBT) {
    const double beats =
        double(pos->beat - 1) + double(pos->tick) / double(pos->ticks_per_beat);
    return beats - double(pos->beats_per_bar);
  } else {
    const double quantum = std::max(m_quantum, 1.0);
    const double beats = m_tempo * pos->frame / (60.0 * pos->frame_rate);
    return std::fmod(beats, quantum) - quantum;
  }
}

void jack_link::worker_start(void) {
  std::unique_lock<std::mutex> lock(m_mutex);

  std::cout << jack_link::name() << " v" << jack_link::version() << std::endl;
  std::cout << jack_link::name() << ": started..." << std::endl;

  m_running = true;

  while (m_running) {
    worker_run();
    m_cond.wait_for(lock, std::chrono::milliseconds(100));
  }

  std::cout << jack_link::name() << ": terminated." << std::endl;
}

void jack_link::worker_run(void) {
  if (m_client && m_npeers > 0) {

    int request = 0;

    double beats_per_minute = 0.0;
    double beats_per_bar = 0.0;
    bool playing_req = false;

    jack_position_t pos;
    const jack_transport_state_t state = ::jack_transport_query(m_client, &pos);

    const bool playing =
        (state == JackTransportRolling || state == JackTransportLooping);

    if ((playing && !m_playing) || (!playing && m_playing)) {
      if (m_playing_req) {
        m_playing_req = false;
      } else {
        playing_req = true;
        ++request;
      }
    }

    if (pos.valid & JackPositionBBT) {
      if (std::abs(m_tempo - pos.beats_per_minute) > 0.01) {
        beats_per_minute = pos.beats_per_minute;
        ++request;
      }
      if (std::abs(m_quantum - pos.beats_per_bar) > 0.01) {
        beats_per_bar = pos.beats_per_bar;
        ++request;
      }
    }

    if (request > 0) {
      auto session_state = m_link.captureAppSessionState();
      const auto host_time = m_link.clock().micros();
      if (beats_per_minute > 0.0) {
        m_tempo = beats_per_minute;
        session_state.setTempo(m_tempo, host_time);
      }
      if (beats_per_bar > 0.0) {
        m_quantum = beats_per_bar;
        // Sync to current JACK transport frame-beat quantum...
        if (m_playing && !playing_req) {
          const double beat = position_beat(&pos);
          session_state.forceBeatAtTime(beat, host_time, m_quantum);
        }
      }
      if (playing_req) {
        m_playing_req = true;
        m_playing = playing;
        // Sync to current JACK transport frame-beat quantum...
        if (m_playing) {
          const double beat = position_beat(&pos);
          session_state.forceBeatAtTime(beat, host_time, m_quantum);
        }
        // Start/stop playing on Link...
        session_state.setIsPlaying(m_playing, host_time);
      }
      m_link.commitAppSessionState(session_state);
    }
  }
}

void jack_link::worker_stop(void) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_running) {
    m_running = false;
    m_cond.notify_all();
  }
}

void trim_ws(std::string &s) {
  const char *ws = " \t\n\r";
  const std::string::size_type first = s.find_first_not_of(ws);
  if (first != std::string::npos)
    s.erase(0, first);
  const std::string::size_type last = s.find_last_not_of(ws);
  if (last != std::string::npos)
    s.erase(last + 1);
  else
    s.clear();
}

const int BUTTON_A = 5;
const int BUTTON_B = 6;
const int BUTTON_X = 16;
const int BUTTON_Y = 24;

void setup_buttons(int pi) {
  set_mode(pi, BUTTON_A, PI_INPUT);
  set_mode(pi, BUTTON_B, PI_INPUT);
  set_mode(pi, BUTTON_X, PI_INPUT);
  set_mode(pi, BUTTON_Y, PI_INPUT);
  set_pull_up_down(pi, BUTTON_Y, PI_PUD_UP);
  set_pull_up_down(pi, BUTTON_B, PI_PUD_UP);
  set_pull_up_down(pi, BUTTON_X, PI_PUD_UP);
  set_pull_up_down(pi, BUTTON_Y, PI_PUD_UP);
}

int main(int /*argc*/, char ** /*argv*/) {
  int pi = pigpio_start(NULL, NULL);

  setup_buttons(pi);
  printf("here: %i", gpio_read(pi, BUTTON_A));
  jack_link app;

  std::string line, arg;

  while (app.active()) {
    bool a_pressed = !gpio_read(pi, BUTTON_A);
    bool b_pressed = !gpio_read(pi, BUTTON_B);
    bool x_pressed = !gpio_read(pi, BUTTON_X);
    bool y_pressed = !gpio_read(pi, BUTTON_Y);
    if (y_pressed) {
      app.playing(true);
    } else if (b_pressed) {
      app.playing(false);
    } else if (x_pressed) {
      app.tempo(app.tempo() + 1);
    } else if (a_pressed) {
      app.tempo(app.tempo() - 1);
    }
  }

  return 0;
}

// end of jack_link.cpp
