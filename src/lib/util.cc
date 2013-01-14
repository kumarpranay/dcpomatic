/*
    Copyright (C) 2012 Carl Hetherington <cth@carlh.net>
    Copyright (C) 2000-2007 Paul Davis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

/** @file src/lib/util.cc
 *  @brief Some utility functions and classes.
 */

#include <sstream>
#include <iomanip>
#include <iostream>
#include <fstream>
#ifdef DVDOMATIC_POSIX
#include <execinfo.h>
#include <cxxabi.h>
#endif
#include <libssh/libssh.h>
#include <signal.h>
#include <boost/algorithm/string.hpp>
#include <boost/bind.hpp>
#include <boost/lambda/lambda.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/thread.hpp>
#include <boost/filesystem.hpp>
#include <openjpeg.h>
#include <openssl/md5.h>
#include <magick/MagickCore.h>
#include <magick/version.h>
#include <libdcp/version.h>
#include <libdcp/util.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavfilter/avfiltergraph.h>
#include <libpostproc/postprocess.h>
#include <libavutil/pixfmt.h>
}
#include "util.h"
#include "exceptions.h"
#include "scaler.h"
#include "format.h"
#include "dcp_content_type.h"
#include "filter.h"
#include "sound_processor.h"

using namespace std;
using namespace boost;

thread::id ui_thread;

/** Convert some number of seconds to a string representation
 *  in hours, minutes and seconds.
 *
 *  @param s Seconds.
 *  @return String of the form H:M:S (where H is hours, M
 *  is minutes and S is seconds).
 */
string
seconds_to_hms (int s)
{
	int m = s / 60;
	s -= (m * 60);
	int h = m / 60;
	m -= (h * 60);

	stringstream hms;
	hms << h << ":";
	hms.width (2);
	hms << setfill ('0') << m << ":";
	hms.width (2);
	hms << setfill ('0') << s;

	return hms.str ();
}

/** @param s Number of seconds.
 *  @return String containing an approximate description of s (e.g. "about 2 hours")
 */
string
seconds_to_approximate_hms (int s)
{
	int m = s / 60;
	s -= (m * 60);
	int h = m / 60;
	m -= (h * 60);

	stringstream ap;
	
	if (h > 0) {
		if (m > 30) {
			ap << (h + 1) << " hours";
		} else {
			if (h == 1) {
				ap << "1 hour";
			} else {
				ap << h << " hours";
			}
		}
	} else if (m > 0) {
		if (m == 1) {
			ap << "1 minute";
		} else {
			ap << m << " minutes";
		}
	} else {
		ap << s << " seconds";
	}

	return ap.str ();
}

#ifdef DVDOMATIC_POSIX
/** @param l Mangled C++ identifier.
 *  @return Demangled version.
 */
static string
demangle (string l)
{
	string::size_type const b = l.find_first_of ("(");
	if (b == string::npos) {
		return l;
	}

	string::size_type const p = l.find_last_of ("+");
	if (p == string::npos) {
		return l;
	}

	if ((p - b) <= 1) {
		return l;
	}
	
	string const fn = l.substr (b + 1, p - b - 1);

	int status;
	try {
		
		char* realname = abi::__cxa_demangle (fn.c_str(), 0, 0, &status);
		string d (realname);
		free (realname);
		return d;
		
	} catch (std::exception) {
		
	}
	
	return l;
}

/** Write a stacktrace to an ostream.
 *  @param out Stream to write to.
 *  @param levels Number of levels to go up the call stack.
 */
void
stacktrace (ostream& out, int levels)
{
	void *array[200];
	size_t size;
	char **strings;
	size_t i;
     
	size = backtrace (array, 200);
	strings = backtrace_symbols (array, size);
     
	if (strings) {
		for (i = 0; i < size && (levels == 0 || i < size_t(levels)); i++) {
			out << "  " << demangle (strings[i]) << endl;
		}
		
		free (strings);
	}
}
#endif

/** @param v Version as used by FFmpeg.
 *  @return A string representation of v.
 */
static string
ffmpeg_version_to_string (int v)
{
	stringstream s;
	s << ((v & 0xff0000) >> 16) << "." << ((v & 0xff00) >> 8) << "." << (v & 0xff);
	return s.str ();
}

/** Return a user-readable string summarising the versions of our dependencies */
string
dependency_version_summary ()
{
	stringstream s;
	s << "libopenjpeg " << opj_version () << ", "
	  << "libavcodec " << ffmpeg_version_to_string (avcodec_version()) << ", "
	  << "libavfilter " << ffmpeg_version_to_string (avfilter_version()) << ", "
	  << "libavformat " << ffmpeg_version_to_string (avformat_version()) << ", "
	  << "libavutil " << ffmpeg_version_to_string (avutil_version()) << ", "
	  << "libpostproc " << ffmpeg_version_to_string (postproc_version()) << ", "
	  << "libswscale " << ffmpeg_version_to_string (swscale_version()) << ", "
	  << MagickVersion << ", "
	  << "libssh " << ssh_version (0) << ", "
	  << "libdcp " << libdcp::version << " git " << libdcp::git_commit;

	return s.str ();
}

double
seconds (struct timeval t)
{
	return t.tv_sec + (double (t.tv_usec) / 1e6);
}

/** Call the required functions to set up DVD-o-matic's static arrays, etc.
 *  Must be called from the UI thread, if there is one.
 */
void
dvdomatic_setup ()
{
	libdcp::init ();
	
	Format::setup_formats ();
	DCPContentType::setup_dcp_content_types ();
	Scaler::setup_scalers ();
	Filter::setup_filters ();
	SoundProcessor::setup_sound_processors ();

	ui_thread = this_thread::get_id ();
}

/** @param start Start position for the crop within the image.
 *  @param size Size of the cropped area.
 *  @return FFmpeg crop filter string.
 */
string
crop_string (Position start, Size size)
{
	stringstream s;
	s << "crop=" << size.width << ":" << size.height << ":" << start.x << ":" << start.y;
	return s.str ();
}

/** @param s A string.
 *  @return Parts of the string split at spaces, except when a space is within quotation marks.
 */
vector<string>
split_at_spaces_considering_quotes (string s)
{
	vector<string> out;
	bool in_quotes = false;
	string c;
	for (string::size_type i = 0; i < s.length(); ++i) {
		if (s[i] == ' ' && !in_quotes) {
			out.push_back (c);
			c = "";
		} else if (s[i] == '"') {
			in_quotes = !in_quotes;
		} else {
			c += s[i];
		}
	}

	out.push_back (c);
	return out;
}

string
md5_digest (void const * data, int size)
{
	MD5_CTX md5_context;
	MD5_Init (&md5_context);
	MD5_Update (&md5_context, data, size);
	unsigned char digest[MD5_DIGEST_LENGTH];
	MD5_Final (digest, &md5_context);
	
	stringstream s;
	for (int i = 0; i < MD5_DIGEST_LENGTH; ++i) {
		s << hex << setfill('0') << setw(2) << ((int) digest[i]);
	}

	return s.str ();
}

/** @param file File name.
 *  @return MD5 digest of file's contents.
 */
string
md5_digest (string file)
{
	ifstream f (file.c_str(), ios::binary);
	if (!f.good ()) {
		throw OpenFileError (file);
	}
	
	f.seekg (0, ios::end);
	int bytes = f.tellg ();
	f.seekg (0, ios::beg);

	int const buffer_size = 64 * 1024;
	char buffer[buffer_size];

	MD5_CTX md5_context;
	MD5_Init (&md5_context);
	while (bytes > 0) {
		int const t = min (bytes, buffer_size);
		f.read (buffer, t);
		MD5_Update (&md5_context, buffer, t);
		bytes -= t;
	}

	unsigned char digest[MD5_DIGEST_LENGTH];
	MD5_Final (digest, &md5_context);

	stringstream s;
	for (int i = 0; i < MD5_DIGEST_LENGTH; ++i) {
		s << hex << setfill('0') << setw(2) << ((int) digest[i]);
	}

	return s.str ();
}

/** @param fps Arbitrary frames-per-second value.
 *  @return DCPFrameRate for this frames-per-second.
 */
DCPFrameRate
dcp_frame_rate (float fps)
{
	DCPFrameRate dfr;

	dfr.run_fast = (fps != rint (fps));
	dfr.frames_per_second = rint (fps);
	dfr.skip = 1;

	/* XXX: somewhat arbitrary */
	if (fps == 50) {
		dfr.frames_per_second = 25;
		dfr.skip = 2;
	}

	return dfr;
}

/** @param An arbitrary sampling rate.
 *  @return The appropriate DCP-approved sampling rate (48kHz or 96kHz).
 */
int
dcp_audio_sample_rate (int fs)
{
	if (fs <= 48000) {
		return 48000;
	}

	return 96000;
}

int
dcp_audio_channels (int f)
{
	if (f == 1) {
		/* The source is mono, so to put the mono channel into
		   the centre we need to generate a 5.1 soundtrack.
		*/
		return 6;
	}

	return f;
}


bool operator== (Size const & a, Size const & b)
{
	return (a.width == b.width && a.height == b.height);
}

bool operator!= (Size const & a, Size const & b)
{
	return !(a == b);
}

bool operator== (Crop const & a, Crop const & b)
{
	return (a.left == b.left && a.right == b.right && a.top == b.top && a.bottom == b.bottom);
}

bool operator!= (Crop const & a, Crop const & b)
{
	return !(a == b);
}

/** @param index Colour LUT index.
 *  @return Human-readable name.
 */
string
colour_lut_index_to_name (int index)
{
	switch (index) {
	case 0:
		return "sRGB";
	case 1:
		return "Rec 709";
	}

	assert (false);
	return "";
}

Socket::Socket ()
	: _deadline (_io_service)
	, _socket (_io_service)
	, _buffer_data (0)
{
	_deadline.expires_at (posix_time::pos_infin);
	check ();
}

void
Socket::check ()
{
	if (_deadline.expires_at() <= asio::deadline_timer::traits_type::now ()) {
		_socket.close ();
		_deadline.expires_at (posix_time::pos_infin);
	}

	_deadline.async_wait (boost::bind (&Socket::check, this));
}

/** Blocking connect with timeout.
 *  @param endpoint End-point to connect to.
 *  @param timeout Time-out in seconds.
 */
void
Socket::connect (asio::ip::basic_resolver_entry<asio::ip::tcp> const & endpoint, int timeout)
{
	_deadline.expires_from_now (posix_time::seconds (timeout));
	system::error_code ec = asio::error::would_block;
	_socket.async_connect (endpoint, lambda::var(ec) = lambda::_1);
	do {
		_io_service.run_one();
	} while (ec == asio::error::would_block);

	if (ec || !_socket.is_open ()) {
		throw NetworkError ("connect timed out");
	}
}

/** Blocking write with timeout.
 *  @param data Buffer to write.
 *  @param size Number of bytes to write.
 *  @param timeout Time-out, in seconds.
 */
void
Socket::write (uint8_t const * data, int size, int timeout)
{
	_deadline.expires_from_now (posix_time::seconds (timeout));
	system::error_code ec = asio::error::would_block;

	asio::async_write (_socket, asio::buffer (data, size), lambda::var(ec) = lambda::_1);
	do {
		_io_service.run_one ();
	} while (ec == asio::error::would_block);

	if (ec) {
		throw NetworkError ("write timed out");
	}
}

/** Blocking read with timeout.
 *  @param data Buffer to read to.
 *  @param size Number of bytes to read.
 *  @param timeout Time-out, in seconds.
 */
int
Socket::read (uint8_t* data, int size, int timeout)
{
	_deadline.expires_from_now (posix_time::seconds (timeout));
	system::error_code ec = asio::error::would_block;

	int amount_read = 0;

	_socket.async_read_some (
		asio::buffer (data, size),
		(lambda::var(ec) = lambda::_1, lambda::var(amount_read) = lambda::_2)
		);

	do {
		_io_service.run_one ();
	} while (ec == asio::error::would_block);
	
	if (ec) {
		amount_read = 0;
	}

	return amount_read;
}

/** Mark some data as being `consumed', so that it will not be returned
 *  as data again.
 *  @param size Amount of data to consume, in bytes.
 */
void
Socket::consume (int size)
{
	assert (_buffer_data >= size);
	
	_buffer_data -= size;
	if (_buffer_data > 0) {
		/* Shift still-valid data to the start of the buffer */
		memmove (_buffer, _buffer + size, _buffer_data);
	}
}

/** Read a definite amount of data from our socket, and mark
 *  it as consumed.
 *  @param data Where to put the data.
 *  @param size Number of bytes to read.
 */
void
Socket::read_definite_and_consume (uint8_t* data, int size, int timeout)
{
	int const from_buffer = min (_buffer_data, size);
	if (from_buffer > 0) {
		/* Get data from our buffer */
		memcpy (data, _buffer, from_buffer);
		consume (from_buffer);
		/* Update our output state */
		data += from_buffer;
		size -= from_buffer;
	}

	/* read() the rest */
	while (size > 0) {
		int const n = read (data, size, timeout);
		if (n <= 0) {
			throw NetworkError ("could not read");
		}

		data += n;
		size -= n;
	}
}

/** Read as much data as is available, up to some limit.
 *  @param data Where to put the data.
 *  @param size Maximum amount of data to read.
 */
void
Socket::read_indefinite (uint8_t* data, int size, int timeout)
{
	assert (size < int (sizeof (_buffer)));

	/* Amount of extra data we need to read () */
	int to_read = size - _buffer_data;
	while (to_read > 0) {
		/* read as much of it as we can (into our buffer) */
		int const n = read (_buffer + _buffer_data, to_read, timeout);
		if (n <= 0) {
			throw NetworkError ("could not read");
		}

		to_read -= n;
		_buffer_data += n;
	}

	assert (_buffer_data >= size);

	/* copy data into the output buffer */
	assert (size >= _buffer_data);
	memcpy (data, _buffer, size);
}

/** @param other A Rect.
 *  @return The intersection of this with `other'.
 */
Rect
Rect::intersection (Rect const & other) const
{
	int const tx = max (x, other.x);
	int const ty = max (y, other.y);
	
	return Rect (
		tx, ty,
		min (x + width, other.x + other.width) - tx,
		min (y + height, other.y + other.height) - ty
		);
}

/** Round a number up to the nearest multiple of another number.
 *  @param c Index.
 *  @param s Array of numbers to round, indexed by c.
 *  @param t Multiple to round to.
 *  @return Rounded number.
 */
int
stride_round_up (int c, int const * stride, int t)
{
	int const a = stride[c] + (t - 1);
	return a - (a % t);
}

int
stride_lookup (int c, int const * stride)
{
	return stride[c];
}

/** Read a sequence of key / value pairs from a text stream;
 *  the keys are the first words on the line, and the values are
 *  the remainder of the line following the key.  Lines beginning
 *  with # are ignored.
 *  @param s Stream to read.
 *  @return key/value pairs.
 */
multimap<string, string>
read_key_value (istream &s) 
{
	multimap<string, string> kv;
	
	string line;
	while (getline (s, line)) {
		if (line.empty ()) {
			continue;
		}

		if (line[0] == '#') {
			continue;
		}

		if (line[line.size() - 1] == '\r') {
			line = line.substr (0, line.size() - 1);
		}

		size_t const s = line.find (' ');
		if (s == string::npos) {
			continue;
		}

		kv.insert (make_pair (line.substr (0, s), line.substr (s + 1)));
	}

	return kv;
}

string
get_required_string (multimap<string, string> const & kv, string k)
{
	if (kv.count (k) > 1) {
		throw StringError ("unexpected multiple keys in key-value set");
	}

	multimap<string, string>::const_iterator i = kv.find (k);
	
	if (i == kv.end ()) {
		throw StringError (String::compose ("missing key %1 in key-value set", k));
	}

	return i->second;
}

int
get_required_int (multimap<string, string> const & kv, string k)
{
	string const v = get_required_string (kv, k);
	return lexical_cast<int> (v);
}

float
get_required_float (multimap<string, string> const & kv, string k)
{
	string const v = get_required_string (kv, k);
	return lexical_cast<float> (v);
}

string
get_optional_string (multimap<string, string> const & kv, string k)
{
	if (kv.count (k) > 1) {
		throw StringError ("unexpected multiple keys in key-value set");
	}

	multimap<string, string>::const_iterator i = kv.find (k);
	if (i == kv.end ()) {
		return "";
	}

	return i->second;
}

int
get_optional_int (multimap<string, string> const & kv, string k)
{
	if (kv.count (k) > 1) {
		throw StringError ("unexpected multiple keys in key-value set");
	}

	multimap<string, string>::const_iterator i = kv.find (k);
	if (i == kv.end ()) {
		return 0;
	}

	return lexical_cast<int> (i->second);
}

/** Construct an AudioBuffers.  Audio data is undefined after this constructor.
 *  @param channels Number of channels.
 *  @param frames Number of frames to reserve space for.
 */
AudioBuffers::AudioBuffers (int channels, int frames)
	: _channels (channels)
	, _frames (frames)
	, _allocated_frames (frames)
{
	_data = new float*[_channels];
	for (int i = 0; i < _channels; ++i) {
		_data[i] = new float[frames];
	}
}

/** Copy constructor.
 *  @param other Other AudioBuffers; data is copied.
 */
AudioBuffers::AudioBuffers (AudioBuffers const & other)
	: _channels (other._channels)
	, _frames (other._frames)
	, _allocated_frames (other._frames)
{
	_data = new float*[_channels];
	for (int i = 0; i < _channels; ++i) {
		_data[i] = new float[_frames];
		memcpy (_data[i], other._data[i], _frames * sizeof (float));
	}
}

/** AudioBuffers destructor */
AudioBuffers::~AudioBuffers ()
{
	for (int i = 0; i < _channels; ++i) {
		delete[] _data[i];
	}

	delete[] _data;
}

/** @param c Channel index.
 *  @return Buffer for this channel.
 */
float*
AudioBuffers::data (int c) const
{
	assert (c >= 0 && c < _channels);
	return _data[c];
}

/** Set the number of frames that these AudioBuffers will report themselves
 *  as having.
 *  @param f Frames; must be less than or equal to the number of allocated frames.
 */
void
AudioBuffers::set_frames (int f)
{
	assert (f <= _allocated_frames);
	_frames = f;
}

/** Make all samples on all channels silent */
void
AudioBuffers::make_silent ()
{
	for (int i = 0; i < _channels; ++i) {
		make_silent (i);
	}
}

/** Make all samples on a given channel silent.
 *  @param c Channel.
 */
void
AudioBuffers::make_silent (int c)
{
	assert (c >= 0 && c < _channels);
	
	for (int i = 0; i < _frames; ++i) {
		_data[c][i] = 0;
	}
}

/** Copy data from another AudioBuffers to this one.  All channels are copied.
 *  @param from AudioBuffers to copy from; must have the same number of channels as this.
 *  @param frames_to_copy Number of frames to copy.
 *  @param read_offset Offset to read from in `from'.
 *  @param write_offset Offset to write to in `to'.
 */
void
AudioBuffers::copy_from (AudioBuffers* from, int frames_to_copy, int read_offset, int write_offset)
{
	assert (from->channels() == channels());

	assert (from);
	assert (read_offset >= 0 && (read_offset + frames_to_copy) <= from->_allocated_frames);
	assert (write_offset >= 0 && (write_offset + frames_to_copy) <= _allocated_frames);

	for (int i = 0; i < _channels; ++i) {
		memcpy (_data[i] + write_offset, from->_data[i] + read_offset, frames_to_copy * sizeof(float));
	}
}

/** Move audio data around.
 *  @param from Offset to move from.
 *  @param to Offset to move to.
 *  @param frames Number of frames to move.
 */
    
void
AudioBuffers::move (int from, int to, int frames)
{
	if (frames == 0) {
		return;
	}
	
	assert (from >= 0);
	assert (from < _frames);
	assert (to >= 0);
	assert (to < _frames);
	assert (frames > 0);
	assert (frames <= _frames);
	assert ((from + frames) <= _frames);
	assert ((to + frames) <= _frames);
	
	for (int i = 0; i < _channels; ++i) {
		memmove (_data[i] + to, _data[i] + from, frames * sizeof(float));
	}
}

/** Trip an assert if the caller is not in the UI thread */
void
ensure_ui_thread ()
{
	assert (this_thread::get_id() == ui_thread);
}

/** @param v Source video frame.
 *  @param audio_sample_rate Source audio sample rate.
 *  @param frames_per_second Number of video frames per second.
 *  @return Equivalent number of audio frames for `v'.
 */
int64_t
video_frames_to_audio_frames (SourceFrame v, float audio_sample_rate, float frames_per_second)
{
	return ((int64_t) v * audio_sample_rate / frames_per_second);
}

/** @param f Filename.
 *  @return true if this file is a still image, false if it is something else.
 */
bool
still_image_file (string f)
{
#if BOOST_FILESYSTEM_VERSION == 3
	string ext = boost::filesystem::path(f).extension().string();
#else
	string ext = boost::filesystem::path(f).extension();
#endif

	transform (ext.begin(), ext.end(), ext.begin(), ::tolower);
	
	return (ext == ".tif" || ext == ".tiff" || ext == ".jpg" || ext == ".jpeg" || ext == ".png");
}

/** @return A pair containing CPU model name and the number of processors */
pair<string, int>
cpu_info ()
{
	pair<string, int> info;
	info.second = 0;
	
#ifdef DVDOMATIC_POSIX
	ifstream f ("/proc/cpuinfo");
	while (f.good ()) {
		string l;
		getline (f, l);
		if (boost::algorithm::starts_with (l, "model name")) {
			string::size_type const c = l.find (':');
			if (c != string::npos) {
				info.first = l.substr (c + 2);
			}
		} else if (boost::algorithm::starts_with (l, "processor")) {
			++info.second;
		}
	}
#endif	

	return info;
}
