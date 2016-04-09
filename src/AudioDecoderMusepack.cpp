/*
  SDL_audiolib - An audio decoding, resampling and mixing library
  Copyright (C) 2014  Nikos Chantziaras

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/
#include "Aulib/AudioDecoderMusepack.h"

#include <cstring>
#include <mpc/reader.h>
#include <mpc/mpcdec.h>
#include <SDL_rwops.h>
#include "aulib_debug.h"
#include "aulib_config.h"
#include "Buffer.h"

#ifdef MPC_FIXED_POINT
#error Fixed point decoder versions of libmpcdec are not supported!
#endif


extern "C" {

static mpc_int32_t
mpcReadCb(mpc_reader* reader, void* ptr, mpc_int32_t size)
{
    return SDL_RWread(static_cast<SDL_RWops*>(reader->data), ptr, 1, size);
}


static mpc_bool_t
mpcSeekCb(mpc_reader* reader, mpc_int32_t offset)
{
    return SDL_RWseek(static_cast<SDL_RWops*>(reader->data), offset, RW_SEEK_SET) >= 0;
}


static mpc_int32_t
mpcTellCb(mpc_reader* reader)
{
    return SDL_RWtell(static_cast<SDL_RWops*>(reader->data));
}


static mpc_int32_t
mpcGetSizeCb(mpc_reader* reader)
{
    return SDL_RWsize(static_cast<SDL_RWops*>(reader->data));
}


static mpc_bool_t
mpcCanseekCb(mpc_reader* reader)
{
    return SDL_RWseek(static_cast<SDL_RWops*>(reader->data), 0, RW_SEEK_CUR) > -1;
}

} // extern "C"


namespace Aulib {

/// \private
struct AudioDecoderMusepack_priv final {
public:
    AudioDecoderMusepack_priv();

    mpc_reader reader;
    std::unique_ptr<mpc_demux, decltype(&mpc_demux_exit)> demuxer;
    mpc_frame_info curFrame;
    Buffer<float> curFrameBuffer;
    mpc_streaminfo strmInfo;
    unsigned frameBufPos;
    bool eof;
    float duration;
};

} // namespace Aulib


Aulib::AudioDecoderMusepack_priv::AudioDecoderMusepack_priv()
    : demuxer(nullptr, &mpc_demux_exit),
      curFrameBuffer(MPC_DECODER_BUFFER_LENGTH),
      frameBufPos(0),
      eof(false),
      duration(-1.f)
{
    reader.read = mpcReadCb;
    reader.seek = mpcSeekCb;
    reader.tell = mpcTellCb;
    reader.get_size = mpcGetSizeCb;
    reader.canseek = mpcCanseekCb;
    reader.data = nullptr;
    curFrame.buffer = curFrameBuffer.get();
    curFrame.samples = 0;
}


Aulib::AudioDecoderMusepack::AudioDecoderMusepack()
    : d(std::make_unique<AudioDecoderMusepack_priv>())
{
}


Aulib::AudioDecoderMusepack::~AudioDecoderMusepack()
{ }


bool
Aulib::AudioDecoderMusepack::open(SDL_RWops* rwops)
{
    if (isOpen()) {
        return true;
    }
    d->reader.data = rwops;
    d->demuxer.reset(mpc_demux_init(&d->reader));
    if (not d->demuxer) {
        d->reader.data = nullptr;
        return false;
    }
    mpc_demux_get_info(d->demuxer.get(), &d->strmInfo);
    setIsOpen(true);
    return true;
}


unsigned
Aulib::AudioDecoderMusepack::getChannels() const
{
    return d->demuxer ? d->strmInfo.channels : 0;
}


unsigned
Aulib::AudioDecoderMusepack::getRate() const
{
    return d->demuxer ? d->strmInfo.sample_freq : 0;
}


size_t
Aulib::AudioDecoderMusepack::doDecoding(float buf[], size_t len, bool& callAgain)
{
    callAgain = false;
    unsigned totalSamples = 0;
    unsigned wantedSamples = len;

    if (d->eof) {
        return 0;
    }

    // If we have any left-over samples from the previous frame, copy them out.
    if (d->curFrame.samples > 0) {
        unsigned copyLen = std::min((size_t)d->curFrame.samples * d->strmInfo.channels, len);
        std::memcpy(buf, d->curFrame.buffer + d->frameBufPos, copyLen * sizeof(*buf));
        d->curFrame.samples -= copyLen / d->strmInfo.channels;
        if (d->curFrame.samples != 0) {
            // There's still more samples left.
            d->frameBufPos += copyLen;
            return copyLen;
        }
        buf += copyLen;
        len -= copyLen;
        totalSamples += copyLen;
        d->frameBufPos = 0;
    }

    // Decode one frame at a time, until we have enough samples.
    while (totalSamples < wantedSamples) {
        if (mpc_demux_decode(d->demuxer.get(), &d->curFrame) != MPC_STATUS_OK) {
            AM_warnLn("AudioDecoderMusepack decoding error.");
            return 0;
        }
        unsigned copyLen = std::min((size_t)d->curFrame.samples * d->strmInfo.channels, len);
        std::memcpy(buf, d->curFrame.buffer, copyLen * sizeof(*buf));
        d->frameBufPos = copyLen;
        d->curFrame.samples -= copyLen / d->strmInfo.channels;
        totalSamples += copyLen;
        len -= copyLen;
        buf += copyLen;
        if (d->curFrame.bits == -1) {
            d->eof = true;
            return totalSamples;
        }
    }
    return totalSamples;
}


bool
Aulib::AudioDecoderMusepack::rewind()
{
    return seekToTime(0);
}


float
Aulib::AudioDecoderMusepack::duration() const
{
    return d->demuxer ? mpc_streaminfo_get_length(&d->strmInfo) : 0;
}


bool
Aulib::AudioDecoderMusepack::seekToTime(float seconds)
{
    if (not d->demuxer) {
        return false;
    }
    mpc_status status = mpc_demux_seek_second(d->demuxer.get(), seconds);
    if (status == MPC_STATUS_OK) {
        d->eof = false;
    }
    return status == MPC_STATUS_OK;
}
