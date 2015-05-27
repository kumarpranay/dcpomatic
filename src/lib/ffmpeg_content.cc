/*
    Copyright (C) 2013-2015 Carl Hetherington <cth@carlh.net>

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

#include "ffmpeg_content.h"
#include "ffmpeg_examiner.h"
#include "ffmpeg_subtitle_stream.h"
#include "ffmpeg_audio_stream.h"
#include "compose.hpp"
#include "job.h"
#include "util.h"
#include "filter.h"
#include "film.h"
#include "log.h"
#include "exceptions.h"
#include "frame_rate_change.h"
#include "safe_stringstream.h"
#include "raw_convert.h"
#include <libcxml/cxml.h>
extern "C" {
#include <libavformat/avformat.h>
}

#include "i18n.h"

#define LOG_GENERAL(...) film->log()->log (String::compose (__VA_ARGS__), Log::TYPE_GENERAL);

using std::string;
using std::vector;
using std::list;
using std::cout;
using std::pair;
using boost::shared_ptr;
using boost::dynamic_pointer_cast;

int const FFmpegContentProperty::SUBTITLE_STREAMS = 100;
int const FFmpegContentProperty::SUBTITLE_STREAM = 101;
int const FFmpegContentProperty::AUDIO_STREAMS = 102;
int const FFmpegContentProperty::AUDIO_STREAM = 103;
int const FFmpegContentProperty::FILTERS = 104;

FFmpegContent::FFmpegContent (shared_ptr<const Film> f, boost::filesystem::path p)
	: Content (f, p)
	, VideoContent (f, p)
	, AudioContent (f, p)
	, SubtitleContent (f, p)
{

}

FFmpegContent::FFmpegContent (shared_ptr<const Film> f, cxml::ConstNodePtr node, int version, list<string>& notes)
	: Content (f, node)
	, VideoContent (f, node, version)
	, AudioContent (f, node)
	, SubtitleContent (f, node, version)
{
	list<cxml::NodePtr> c = node->node_children ("SubtitleStream");
	for (list<cxml::NodePtr>::const_iterator i = c.begin(); i != c.end(); ++i) {
		_subtitle_streams.push_back (shared_ptr<FFmpegSubtitleStream> (new FFmpegSubtitleStream (*i)));
		if ((*i)->optional_number_child<int> ("Selected")) {
			_subtitle_stream = _subtitle_streams.back ();
		}
	}

	c = node->node_children ("AudioStream");
	for (list<cxml::NodePtr>::const_iterator i = c.begin(); i != c.end(); ++i) {
		_audio_streams.push_back (shared_ptr<FFmpegAudioStream> (new FFmpegAudioStream (*i, version)));
		if ((*i)->optional_number_child<int> ("Selected")) {
			_audio_stream = _audio_streams.back ();
		}
	}

	c = node->node_children ("Filter");
	for (list<cxml::NodePtr>::iterator i = c.begin(); i != c.end(); ++i) {
		Filter const * f = Filter::from_id ((*i)->content ());
		if (f) {
			_filters.push_back (f);
		} else {
			notes.push_back (String::compose (_("DCP-o-matic no longer supports the `%1' filter, so it has been turned off."), (*i)->content()));
		}
	}

	_first_video = node->optional_number_child<double> ("FirstVideo");
}

FFmpegContent::FFmpegContent (shared_ptr<const Film> f, vector<boost::shared_ptr<Content> > c)
	: Content (f, c)
	, VideoContent (f, c)
	, AudioContent (f, c)
	, SubtitleContent (f, c)
{
	shared_ptr<FFmpegContent> ref = dynamic_pointer_cast<FFmpegContent> (c[0]);
	DCPOMATIC_ASSERT (ref);

	for (size_t i = 0; i < c.size(); ++i) {
		shared_ptr<FFmpegContent> fc = dynamic_pointer_cast<FFmpegContent> (c[i]);
		if (fc->use_subtitles() && *(fc->_subtitle_stream.get()) != *(ref->_subtitle_stream.get())) {
			throw JoinError (_("Content to be joined must use the same subtitle stream."));
		}

		if (*(fc->_audio_stream.get()) != *(ref->_audio_stream.get())) {
			throw JoinError (_("Content to be joined must use the same audio stream."));
		}
	}

	_subtitle_streams = ref->subtitle_streams ();
	_subtitle_stream = ref->subtitle_stream ();
	_audio_streams = ref->audio_streams ();
	_audio_stream = ref->audio_stream ();
	_first_video = ref->_first_video;
}

void
FFmpegContent::as_xml (xmlpp::Node* node) const
{
	node->add_child("Type")->add_child_text ("FFmpeg");
	Content::as_xml (node);
	VideoContent::as_xml (node);
	AudioContent::as_xml (node);
	SubtitleContent::as_xml (node);

	boost::mutex::scoped_lock lm (_mutex);

	for (vector<shared_ptr<FFmpegSubtitleStream> >::const_iterator i = _subtitle_streams.begin(); i != _subtitle_streams.end(); ++i) {
		xmlpp::Node* t = node->add_child("SubtitleStream");
		if (_subtitle_stream && *i == _subtitle_stream) {
			t->add_child("Selected")->add_child_text("1");
		}
		(*i)->as_xml (t);
	}

	for (vector<shared_ptr<FFmpegAudioStream> >::const_iterator i = _audio_streams.begin(); i != _audio_streams.end(); ++i) {
		xmlpp::Node* t = node->add_child("AudioStream");
		if (_audio_stream && *i == _audio_stream) {
			t->add_child("Selected")->add_child_text("1");
		}
		(*i)->as_xml (t);
	}

	for (vector<Filter const *>::const_iterator i = _filters.begin(); i != _filters.end(); ++i) {
		node->add_child("Filter")->add_child_text ((*i)->id ());
	}

	if (_first_video) {
		node->add_child("FirstVideo")->add_child_text (raw_convert<string> (_first_video.get().get()));
	}
}

void
FFmpegContent::examine (shared_ptr<Job> job)
{
	job->set_progress_unknown ();

	Content::examine (job);

	shared_ptr<FFmpegExaminer> examiner (new FFmpegExaminer (shared_from_this (), job));
	take_from_video_examiner (examiner);

	shared_ptr<const Film> film = _film.lock ();
	DCPOMATIC_ASSERT (film);

	{
		boost::mutex::scoped_lock lm (_mutex);

		_subtitle_streams = examiner->subtitle_streams ();
		if (!_subtitle_streams.empty ()) {
			_subtitle_stream = _subtitle_streams.front ();
		}
		
		_audio_streams = examiner->audio_streams ();
		if (!_audio_streams.empty ()) {
			_audio_stream = _audio_streams.front ();
		}

		_first_video = examiner->first_video ();
	}

	signal_changed (FFmpegContentProperty::SUBTITLE_STREAMS);
	signal_changed (FFmpegContentProperty::SUBTITLE_STREAM);
	signal_changed (FFmpegContentProperty::AUDIO_STREAMS);
	signal_changed (FFmpegContentProperty::AUDIO_STREAM);
	signal_changed (AudioContentProperty::AUDIO_CHANNELS);
}

string
FFmpegContent::summary () const
{
	/* Get the string() here so that the name does not have quotes around it */
	return String::compose (_("%1 [movie]"), path_summary ());
}

string
FFmpegContent::technical_summary () const
{
	string as = "none";
	if (_audio_stream) {
		as = _audio_stream->technical_summary ();
	}

	string ss = "none";
	if (_subtitle_stream) {
		ss = _subtitle_stream->technical_summary ();
	}

	string filt = Filter::ffmpeg_string (_filters);
	
	return Content::technical_summary() + " - "
		+ VideoContent::technical_summary() + " - "
		+ AudioContent::technical_summary() + " - "
		+ String::compose (
			"ffmpeg: audio %1, subtitle %2, filters %3", as, ss, filt
			);
}

void
FFmpegContent::set_subtitle_stream (shared_ptr<FFmpegSubtitleStream> s)
{
	{
		boost::mutex::scoped_lock lm (_mutex);
		_subtitle_stream = s;
	}

	signal_changed (FFmpegContentProperty::SUBTITLE_STREAM);
}

void
FFmpegContent::set_audio_stream (shared_ptr<FFmpegAudioStream> s)
{
	{
		boost::mutex::scoped_lock lm (_mutex);
		_audio_stream = s;
	}

	signal_changed (FFmpegContentProperty::AUDIO_STREAM);
}

int
FFmpegContent::audio_channels () const
{
	boost::mutex::scoped_lock lm (_mutex);
	
	if (!_audio_stream) {
		return 0;
	}

	return _audio_stream->channels ();
}

int
FFmpegContent::audio_frame_rate () const
{
	boost::mutex::scoped_lock lm (_mutex);

	if (!_audio_stream) {
		return 0;
	}

	return _audio_stream->frame_rate ();
}

bool
operator== (FFmpegStream const & a, FFmpegStream const & b)
{
	return a._id == b._id;
}

bool
operator!= (FFmpegStream const & a, FFmpegStream const & b)
{
	return a._id != b._id;
}

DCPTime
FFmpegContent::full_length () const
{
	shared_ptr<const Film> film = _film.lock ();
	DCPOMATIC_ASSERT (film);
	FrameRateChange const frc (video_frame_rate (), film->video_frame_rate ());
	return DCPTime::from_frames (rint (video_length_after_3d_combine() * frc.factor()), film->video_frame_rate());
}

AudioMapping
FFmpegContent::audio_mapping () const
{
	boost::mutex::scoped_lock lm (_mutex);

	if (!_audio_stream) {
		return AudioMapping ();
	}

	return _audio_stream->mapping ();
}

void
FFmpegContent::set_filters (vector<Filter const *> const & filters)
{
	{
		boost::mutex::scoped_lock lm (_mutex);
		_filters = filters;
	}

	signal_changed (FFmpegContentProperty::FILTERS);
}

void
FFmpegContent::set_audio_mapping (AudioMapping m)
{
	audio_stream()->set_mapping (m);
	AudioContent::set_audio_mapping (m);
}

string
FFmpegContent::identifier () const
{
	SafeStringStream s;

	s << VideoContent::identifier();

	boost::mutex::scoped_lock lm (_mutex);

	if (_subtitle_stream) {
		s << "_" << _subtitle_stream->identifier ();
	}

	for (vector<Filter const *>::const_iterator i = _filters.begin(); i != _filters.end(); ++i) {
		s << "_" << (*i)->id ();
	}

	return s.str ();
}

boost::filesystem::path
FFmpegContent::audio_analysis_path () const
{
	shared_ptr<const Film> film = _film.lock ();
	if (!film) {
		return boost::filesystem::path ();
	}

	/* We need to include the stream ID in this path so that we get different
	   analyses for each stream.
	*/

	boost::filesystem::path p = AudioContent::audio_analysis_path ();
	if (audio_stream ()) {
		p = p.string() + "_" + audio_stream()->identifier ();
	}
	return p;
}

list<ContentTimePeriod>
FFmpegContent::subtitles_during (ContentTimePeriod period, bool starting) const
{
	shared_ptr<FFmpegSubtitleStream> stream = subtitle_stream ();
	if (!stream) {
		return list<ContentTimePeriod> ();
	}

	return stream->subtitles_during (period, starting);
}

bool
FFmpegContent::has_subtitles () const
{
	return !subtitle_streams().empty ();
}

void
FFmpegContent::set_default_colour_conversion ()
{
	dcp::Size const s = video_size ();

	boost::mutex::scoped_lock lm (_mutex);

	if (s.width < 1080) {
		_colour_conversion = PresetColourConversion::from_id ("rec601").conversion;
	} else {
		_colour_conversion = PresetColourConversion::from_id ("rec709").conversion;
	}
}


