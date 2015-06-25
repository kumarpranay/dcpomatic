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

#include "player_video.h"
#include "image.h"
#include "image_proxy.h"
#include "j2k_image_proxy.h"
#include "film.h"
#include "raw_convert.h"

using std::string;
using std::cout;
using boost::shared_ptr;
using boost::dynamic_pointer_cast;
using boost::optional;

PlayerVideo::PlayerVideo (
	shared_ptr<const ImageProxy> in,
	DCPTime time,
	Crop crop,
	boost::optional<float> fade,
	dcp::Size inter_size,
	dcp::Size out_size,
	Eyes eyes,
	Part part,
	optional<ColourConversion> colour_conversion
	)
	: _in (in)
	, _time (time)
	, _crop (crop)
	, _fade (fade)
	, _inter_size (inter_size)
	, _out_size (out_size)
	, _eyes (eyes)
	, _part (part)
	, _colour_conversion (colour_conversion)
{

}

PlayerVideo::PlayerVideo (shared_ptr<cxml::Node> node, shared_ptr<Socket> socket)
{
	_time = DCPTime (node->number_child<DCPTime::Type> ("Time"));
	_crop = Crop (node);
	_fade = node->optional_number_child<float> ("Fade");

	_inter_size = dcp::Size (node->number_child<int> ("InterWidth"), node->number_child<int> ("InterHeight"));
	_out_size = dcp::Size (node->number_child<int> ("OutWidth"), node->number_child<int> ("OutHeight"));
	_eyes = (Eyes) node->number_child<int> ("Eyes");
	_part = (Part) node->number_child<int> ("Part");

	/* Assume that the ColourConversion uses the current state version */
	_colour_conversion = ColourConversion::from_xml (node, Film::current_state_version);

	_in = image_proxy_factory (node->node_child ("In"), socket);

	if (node->optional_number_child<int> ("SubtitleX")) {

		shared_ptr<Image> image (
			new Image (PIX_FMT_RGBA, dcp::Size (node->number_child<int> ("SubtitleWidth"), node->number_child<int> ("SubtitleHeight")), true)
			);

		image->read_from_socket (socket);

		_subtitle = PositionImage (image, Position<int> (node->number_child<int> ("SubtitleX"), node->number_child<int> ("SubtitleY")));
	}
}

void
PlayerVideo::set_subtitle (PositionImage image)
{
	_subtitle = image;
}

shared_ptr<Image>
PlayerVideo::image (AVPixelFormat pixel_format, dcp::NoteHandler note) const
{
	shared_ptr<Image> im = _in->image (optional<dcp::NoteHandler> (note));

	Crop total_crop = _crop;
	switch (_part) {
	case PART_LEFT_HALF:
		total_crop.right += im->size().width / 2;
		break;
	case PART_RIGHT_HALF:
		total_crop.left += im->size().width / 2;
		break;
	case PART_TOP_HALF:
		total_crop.bottom += im->size().height / 2;
		break;
	case PART_BOTTOM_HALF:
		total_crop.top += im->size().height / 2;
		break;
	default:
		break;
	}

	dcp::YUVToRGB yuv_to_rgb = dcp::YUV_TO_RGB_REC601;
	if (_colour_conversion) {
		yuv_to_rgb = _colour_conversion.get().yuv_to_rgb();
	}

	shared_ptr<Image> out = im->crop_scale_window (total_crop, _inter_size, _out_size, yuv_to_rgb, pixel_format, true);

	if (_subtitle) {
		out->alpha_blend (_subtitle->image, _subtitle->position);
	}

	if (_fade) {
		out->fade (_fade.get ());
	}

	return out;
}

void
PlayerVideo::add_metadata (xmlpp::Node* node) const
{
	node->add_child("Time")->add_child_text (raw_convert<string> (_time.get ()));
	_crop.as_xml (node);
	if (_fade) {
		node->add_child("Fade")->add_child_text (raw_convert<string> (_fade.get ()));
	}
	_in->add_metadata (node->add_child ("In"));
	node->add_child("InterWidth")->add_child_text (raw_convert<string> (_inter_size.width));
	node->add_child("InterHeight")->add_child_text (raw_convert<string> (_inter_size.height));
	node->add_child("OutWidth")->add_child_text (raw_convert<string> (_out_size.width));
	node->add_child("OutHeight")->add_child_text (raw_convert<string> (_out_size.height));
	node->add_child("Eyes")->add_child_text (raw_convert<string> (_eyes));
	node->add_child("Part")->add_child_text (raw_convert<string> (_part));
	if (_colour_conversion) {
		_colour_conversion.get().as_xml (node);
	}
	if (_subtitle) {
		node->add_child ("SubtitleWidth")->add_child_text (raw_convert<string> (_subtitle->image->size().width));
		node->add_child ("SubtitleHeight")->add_child_text (raw_convert<string> (_subtitle->image->size().height));
		node->add_child ("SubtitleX")->add_child_text (raw_convert<string> (_subtitle->position.x));
		node->add_child ("SubtitleY")->add_child_text (raw_convert<string> (_subtitle->position.y));
	}
}

void
PlayerVideo::send_binary (shared_ptr<Socket> socket) const
{
	_in->send_binary (socket);
	if (_subtitle) {
		_subtitle->image->write_to_socket (socket);
	}
}

bool
PlayerVideo::has_j2k () const
{
	/* XXX: burnt-in subtitle; maybe other things */

	shared_ptr<const J2KImageProxy> j2k = dynamic_pointer_cast<const J2KImageProxy> (_in);
	if (!j2k) {
		return false;
	}

	return _crop == Crop () && _inter_size == j2k->size();
}

Data
PlayerVideo::j2k () const
{
	shared_ptr<const J2KImageProxy> j2k = dynamic_pointer_cast<const J2KImageProxy> (_in);
	DCPOMATIC_ASSERT (j2k);
	return j2k->j2k ();
}

Position<int>
PlayerVideo::inter_position () const
{
	return Position<int> ((_out_size.width - _inter_size.width) / 2, (_out_size.height - _inter_size.height) / 2);
}

/** @return true if this PlayerVideo is definitely the same as another
 * (apart from _time), false if it is probably not
 */
bool
PlayerVideo::same (shared_ptr<const PlayerVideo> other) const
{
	if (_crop != other->_crop ||
	    _fade.get_value_or(0) != other->_fade.get_value_or(0) ||
	    _inter_size != other->_inter_size ||
	    _out_size != other->_out_size ||
	    _eyes != other->_eyes ||
	    _part != other->_part ||
	    _colour_conversion != other->_colour_conversion) {
		return false;
	}

	if ((!_subtitle && other->_subtitle) || (_subtitle && !other->_subtitle)) {
		/* One has a subtitle and the other doesn't */
		return false;
	}

	if (_subtitle && other->_subtitle && !_subtitle->same (other->_subtitle.get ())) {
		/* They both have subtitles but they are different */
		return false;
	}

	/* Now neither has subtitles */

	return _in->same (other->_in);
}
