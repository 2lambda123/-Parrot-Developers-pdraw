/**
 * Parrot Drones Awesome Video Viewer Library
 * Video scaler element
 *
 * Copyright (c) 2018 Parrot Drones SAS
 * Copyright (c) 2016 Aurelien Barre
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of the copyright holders nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define ULOG_TAG pdraw_vscale
#include <ulog.h>
ULOG_DECLARE_TAG(ULOG_TAG);

#include "pdraw_scaler_video.hpp"
#include "pdraw_session.hpp"
#include "pdraw_utils.hpp"

#include <time.h>
#include <unistd.h>

#include <vector>

namespace Pdraw {


const struct vscale_cbs VideoScaler::mScalerCbs = {
	.frame_output = &VideoScaler::frameOutputCb,
	.flush = &VideoScaler::flushCb,
	.stop = &VideoScaler::stopCb,
};


VideoScaler::VideoScaler(Session *session,
			 Element::Listener *elementListener,
			 Source::Listener *sourceListener) :
		FilterElement(session,
			      elementListener,
			      1,
			      nullptr,
			      0,
			      nullptr,
			      0,
			      1,
			      sourceListener),
		mInputMedia(nullptr), mOutputMedia(nullptr),
		mInputBufferPool(nullptr), mInputBufferQueue(nullptr),
		mVscale(nullptr), mIsFlushed(true),
		mInputChannelFlushPending(false), mVscaleFlushPending(false),
		mVscaleStopPending(false)
{
	const struct vdef_raw_format *supportedInputFormats;
	int supportedInputFormatsCount;

	Element::setClassName(__func__);

	/* Supported input formats */
	supportedInputFormatsCount = vscale_get_supported_input_formats(
		VSCALE_SCALER_IMPLEM_AUTO, &supportedInputFormats);
	if (supportedInputFormatsCount < 0)
		PDRAW_LOG_ERRNO("vscale_get_supported_input_formats",
				-supportedInputFormatsCount);
	else
		setRawVideoMediaFormatCaps(supportedInputFormats,
					   supportedInputFormatsCount);

	setState(CREATED);
}


VideoScaler::~VideoScaler(void)
{
	int ret;

	if (mState != STOPPED)
		PDRAW_LOGW("scaler is still running");

	/* Remove any leftover idle callbacks */
	ret = pomp_loop_idle_remove_by_cookie(mSession->getLoop(), this);
	if (ret < 0)
		PDRAW_LOG_ERRNO("pomp_loop_idle_remove_by_cookie", -ret);

	if (mVscale) {
		ret = vscale_destroy(mVscale);
		if (ret < 0)
			PDRAW_LOG_ERRNO("vscale_destroy", -ret);
	}

	if (mOutputMedia != nullptr)
		PDRAW_LOGW("output media was not properly removed");
}


int VideoScaler::start(void)
{
	int ret = 0;

	if ((mState == STARTED) || (mState == STARTING)) {
		return 0;
	}
	if (mState != CREATED) {
		PDRAW_LOGE("%s: scaler is not created", __func__);
		return -EPROTO;
	}
	setState(STARTING);

	/* Get the input media and port */
	Sink::lock();
	unsigned int inputMediaCount = getInputMediaCount();
	if (inputMediaCount != 1) {
		Sink::unlock();
		PDRAW_LOGE("invalid input media count");
		return -EPROTO;
	}
	Media *media = getInputMedia(0);
	if (media == nullptr) {
		Sink::unlock();
		PDRAW_LOGE("invalid input media");
		return -EPROTO;
	}
	mInputMedia = dynamic_cast<RawVideoMedia *>(media);
	if (mInputMedia == nullptr) {
		Sink::unlock();
		PDRAW_LOGE("invalid input media");
		return -EPROTO;
	}
	InputPort *port = getInputPort(mInputMedia);
	if (port == nullptr) {
		Sink::unlock();
		PDRAW_LOGE("invalid input port");
		return -EPROTO;
	}

	/* Initialize the scaler */
	struct vscale_config cfg = {};
	cfg.implem = VSCALE_SCALER_IMPLEM_AUTO;
	cfg.input.format = mInputMedia->format;
	cfg.input.info = mInputMedia->info;
	cfg.output.info = cfg.input.info;
	cfg.output.info.resolution.width = 1280; /* TODO */
	cfg.output.info.resolution.height = 720; /* TODO */
	ret = vscale_new(
		mSession->getLoop(), &cfg, &mScalerCbs, this, &mVscale);
	if (ret < 0) {
		Sink::unlock();
		PDRAW_LOG_ERRNO("vscale_new", -ret);
		return ret;
	}

	/* Setup the input port */
	Channel *c = port->channel;
	RawVideoChannel *channel = dynamic_cast<RawVideoChannel *>(c);
	if (channel == nullptr) {
		Sink::unlock();
		PDRAW_LOGE("invalid input channel");
		return -EPROTO;
	}
	mInputBufferQueue = vscale_get_input_buffer_queue(mVscale);
	channel->setQueue(this, mInputBufferQueue);
	mInputBufferPool = vscale_get_input_buffer_pool(mVscale);
	channel->setPool(this, mInputBufferPool);

	Sink::unlock();

	setState(STARTED);

	return 0;
}


int VideoScaler::stop(void)
{
	int ret;

	if ((mState == STOPPED) || (mState == STOPPING))
		return 0;
	if (mState != STARTED) {
		PDRAW_LOGE("%s: scaler is not started", __func__);
		return -EPROTO;
	}
	setState(STOPPING);
	mVscaleStopPending = true;

	/* Flush everything */
	ret = flush();
	if (ret < 0)
		PDRAW_LOG_ERRNO("flush", -ret);

	/* When the flush is complete, stopping will be triggered */
	return ret;
}


int VideoScaler::flush(void)
{
	int ret;
	unsigned int outputChannelCount, i;
	Channel *outputChannel;

	if (mIsFlushed) {
		PDRAW_LOGD("scaler is already flushed, nothing to do");
		int err = pomp_loop_idle_add_with_cookie(
			mSession->getLoop(), &idleCompleteFlush, this, this);
		if (err < 0)
			PDRAW_LOG_ERRNO("pomp_loop_idle_add_with_cookie", -err);
		return 0;
	}

	mVscaleFlushPending = true;

	/* Flush the output channels (async) */
	Source::lock();
	if (mOutputMedia != nullptr) {
		outputChannelCount = getOutputChannelCount(mOutputMedia);
		for (i = 0; i < outputChannelCount; i++) {
			outputChannel = getOutputChannel(mOutputMedia, i);
			if (outputChannel == nullptr) {
				PDRAW_LOGW(
					"failed to get output channel "
					"at index %d",
					i);
				continue;
			}
			ret = outputChannel->flush();
			if (ret < 0)
				PDRAW_LOG_ERRNO("channel->flush", -ret);
		}
	}
	Source::unlock();

	/* Flush the scaler (async)
	 * (the input channel queue is flushed by vscale) */
	ret = vscale_flush(mVscale, 1);
	if (ret < 0)
		PDRAW_LOG_ERRNO("vscale_flush", -ret);

	return ret;
}


void VideoScaler::completeFlush(void)
{
	int ret;
	unsigned int outputChannelCount, i;
	Channel *outputChannel;
	bool pending = false;

	if (mVscaleFlushPending)
		return;

	Source::lock();
	if (mOutputMedia != nullptr) {
		outputChannelCount = getOutputChannelCount(mOutputMedia);
		for (i = 0; i < outputChannelCount; i++) {
			outputChannel = getOutputChannel(mOutputMedia, i);
			if (outputChannel == nullptr) {
				PDRAW_LOGW(
					"failed to get output channel "
					"at index %d",
					i);
				continue;
			}
			if (outputChannel->isFlushPending()) {
				pending = true;
				break;
			}
		}
	}
	Source::unlock();

	if (pending)
		return;

	Sink::lock();
	if (mInputMedia != nullptr) {
		mIsFlushed = true;
		if (mInputChannelFlushPending) {
			mInputChannelFlushPending = false;
			Channel *inputChannel = getInputChannel(mInputMedia);
			if (inputChannel == nullptr) {
				PDRAW_LOGE("failed to get input channel");
			} else {
				ret = inputChannel->flushDone();
				if (ret < 0)
					PDRAW_LOG_ERRNO("channel->flushDone",
							-ret);
			}
		}
	}
	Sink::unlock();

	tryStop();
}


void VideoScaler::idleCompleteFlush(void *userdata)
{
	VideoScaler *self = (VideoScaler *)userdata;
	self->completeFlush();
}


int VideoScaler::tryStop(void)
{
	int ret;
	int outputChannelCount = 0, i;
	Channel *channel;

	if (mState != STOPPING)
		return 0;

	/* Remove the input port */
	Sink::lock();
	if (mInputMedia != nullptr) {
		Channel *c = getInputChannel(mInputMedia);
		RawVideoChannel *channel = dynamic_cast<RawVideoChannel *>(c);
		if (channel == nullptr) {
			PDRAW_LOGE("failed to get channel");
		} else {
			channel->setQueue(this, nullptr);
			channel->setPool(this, nullptr);
		}

		ret = removeInputMedia(mInputMedia);
		if (ret < 0)
			PDRAW_LOG_ERRNO("removeInputMedia", -ret);
		else
			mInputMedia = nullptr;
	}
	Sink::unlock();

	/* Teardown the output channels
	 * Note: loop downwards because calling teardown on a channel may or
	 * may not synchronously remove the channel from the output port */
	Source::lock();
	if (mOutputMedia != nullptr) {
		outputChannelCount = getOutputChannelCount(mOutputMedia);

		for (i = outputChannelCount - 1; i >= 0; i--) {
			channel = getOutputChannel(mOutputMedia, i);
			if (channel == nullptr) {
				PDRAW_LOGW("failed to get channel at index %d",
					   i);
				continue;
			}
			ret = channel->teardown();
			if (ret < 0)
				PDRAW_LOG_ERRNO("channel->teardown", -ret);
		}
	}
	Source::unlock();

	/* Stop the scaler */
	ret = vscale_stop(mVscale);
	if (ret < 0) {
		PDRAW_LOG_ERRNO("vscale_stop", -ret);
		return ret;
	}

	return 0;
}


void VideoScaler::completeStop(void)
{
	int ret;
	unsigned int outputChannelCount;

	Source::lock();
	if (mOutputMedia == nullptr) {
		Source::unlock();
		goto exit;
	}
	outputChannelCount = getOutputChannelCount(mOutputMedia);
	if (outputChannelCount > 0) {
		Source::unlock();
		return;
	}

	/* Remove the output port */
	if (Source::mListener) {
		Source::mListener->onOutputMediaRemoved(
			this, mOutputMedia, nullptr);
	}
	ret = removeOutputPort(mOutputMedia);
	if (ret < 0) {
		PDRAW_LOG_ERRNO("removeOutputPort", -ret);
	} else {
		delete mOutputMedia;
		mOutputMedia = nullptr;
	}

	Source::unlock();

exit:
	if ((!mVscaleStopPending) && (mOutputMedia == nullptr))
		setState(STOPPED);
}


int VideoScaler::createOutputMedia(struct vdef_raw_frame *frameInfo,
				   RawVideoMedia::Frame &frame)
{
	int ret;

	Source::lock();

	mOutputMedia = new RawVideoMedia(mSession);
	if (mOutputMedia == nullptr) {
		Source::unlock();
		PDRAW_LOGE("output media allocation failed");
		return -ENOMEM;
	}
	std::string path = mInputMedia->getPath() + ">" + Element::getName() +
			   "$" + mOutputMedia->getName();
	mOutputMedia->setPath(path);

	ret = addOutputPort(mOutputMedia);
	if (ret < 0) {
		Source::unlock();
		PDRAW_LOG_ERRNO("addOutputPort", -ret);
		return ret;
	}

	mOutputMedia->format = frameInfo->format;
	vdef_frame_to_format_info(&frameInfo->info, &mOutputMedia->info);
	mOutputMedia->info.framerate = mInputMedia->info.framerate;
	mOutputMedia->sessionMeta = mInputMedia->sessionMeta;
	mOutputMedia->playbackType = mInputMedia->playbackType;
	mOutputMedia->duration = mInputMedia->duration;

	Source::unlock();

	if (Source::mListener)
		Source::mListener->onOutputMediaAdded(
			this, mOutputMedia, nullptr);

	return 0;
}


void VideoScaler::onRawVideoChannelQueue(RawVideoChannel *channel,
					 struct mbuf_raw_video_frame *frame)
{

	if (channel == nullptr) {
		PDRAW_LOG_ERRNO("channel", EINVAL);
		return;
	}
	if (frame == nullptr) {
		PDRAW_LOG_ERRNO("frame", EINVAL);
		return;
	}
	if (mState != STARTED) {
		PDRAW_LOGE("frame input: scaler is not started");
		return;
	}
	if ((mVscaleFlushPending) || (mInputChannelFlushPending)) {
		PDRAW_LOGI("frame input: flush pending, discard frame");
		return;
	}
	Sink::lock();
	struct mbuf_raw_video_frame_queue *queue = channel->getQueue(this);
	if (queue == nullptr) {
		Sink::unlock();
		PDRAW_LOGE("invalid queue");
		return;
	}
	if (queue != mInputBufferQueue) {
		Sink::unlock();
		PDRAW_LOGE("invalid input buffer queue");
		return;
	}

	Sink::onRawVideoChannelQueue(channel, frame);
	mIsFlushed = false;
	Sink::unlock();
}


void VideoScaler::onChannelFlush(Channel *channel)
{
	if (channel == nullptr) {
		PDRAW_LOG_ERRNO("channel", EINVAL);
		return;
	}

	PDRAW_LOGD("flushing input channel");
	mInputChannelFlushPending = true;

	int ret = flush();
	if (ret < 0)
		PDRAW_LOG_ERRNO("flush", -ret);
}


void VideoScaler::onChannelFlushed(Channel *channel)
{
	if (channel == nullptr) {
		PDRAW_LOG_ERRNO("channel", EINVAL);
		return;
	}

	Media *media = getOutputMediaFromChannel(channel);
	if (media == nullptr) {
		PDRAW_LOGE("media not found");
		return;
	}
	PDRAW_LOGD("'%s': channel flushed media name=%s (channel owner=%p)",
		   Element::getName().c_str(),
		   media->getName().c_str(),
		   channel->getOwner());

	completeFlush();
}


void VideoScaler::onChannelTeardown(Channel *channel)
{
	if (channel == nullptr) {
		PDRAW_LOG_ERRNO("channel", EINVAL);
		return;
	}

	PDRAW_LOGD("tearing down input channel");

	int ret = stop();
	if (ret < 0)
		PDRAW_LOG_ERRNO("stop", -ret);
}


void VideoScaler::onChannelUnlink(Channel *channel)
{
	if (channel == nullptr) {
		PDRAW_LOG_ERRNO("channel", EINVAL);
		return;
	}

	Source::onChannelUnlink(channel);

	if (mState == STOPPING)
		completeStop();
}


void VideoScaler::onChannelSessionMetaUpdate(Channel *channel)
{
	struct vmeta_session tmpSessionMeta;

	if (channel == nullptr) {
		PDRAW_LOG_ERRNO("channel", EINVAL);
		return;
	}

	Sink::lock();
	if (mInputMedia == nullptr) {
		Sink::unlock();
		PDRAW_LOGE("input media not found");
		return;
	}
	tmpSessionMeta = mInputMedia->sessionMeta;
	Sink::unlock();

	Source::lock();
	if (mOutputMedia == nullptr) {
		Source::unlock();
		PDRAW_LOGE("output media not found");
		return;
	}
	mOutputMedia->sessionMeta = tmpSessionMeta;
	Source::unlock();

	PDRAW_LOGD("updating session metadata");

	FilterElement::onChannelSessionMetaUpdate(channel);
}


void VideoScaler::frameOutputCb(struct vscale_scaler *scaler,
				int status,
				struct mbuf_raw_video_frame *out_frame,
				void *userdata)
{
	int ret;
	VideoScaler *self = (VideoScaler *)userdata;
	struct vdef_raw_frame info;
	struct mbuf_ancillary_data *ancillaryData = nullptr;
	RawVideoMedia::Frame *in_meta;
	RawVideoMedia::Frame out_meta;
	unsigned int outputChannelCount, i;

	if (status != 0) {
		PDRAW_LOGE("scaler error: %d(%s)", -status, strerror(-status));
		return;
	}

	if (userdata == nullptr) {
		PDRAW_LOG_ERRNO("userdata", EINVAL);
		return;
	}
	if (out_frame == nullptr) {
		PDRAW_LOG_ERRNO("out_frame", EINVAL);
		return;
	}
	if (self->mState != STARTED) {
		PDRAW_LOGE("frame output: scaler is not started");
		return;
	}
	if ((self->mVscaleFlushPending) || (self->mInputChannelFlushPending)) {
		PDRAW_LOGI("frame output: flush pending, discard frame");
		return;
	}
	ret = mbuf_raw_video_frame_get_frame_info(out_frame, &info);
	if (ret < 0) {
		PDRAW_LOG_ERRNO("mbuf_raw_video_frame_get_frame_info", -ret);
		return;
	}

	self->Sink::lock();
	if (self->mInputMedia == nullptr) {
		self->Sink::unlock();
		PDRAW_LOG_ERRNO("invalid input media", EPROTO);
		return;
	}

	ret = mbuf_raw_video_frame_get_ancillary_data(
		out_frame,
		PDRAW_ANCILLARY_DATA_KEY_RAWVIDEOFRAME,
		&ancillaryData);
	if (ret < 0) {
		self->Sink::unlock();
		PDRAW_LOG_ERRNO("mbuf_raw_video_frame_get_ancillary_data",
				-ret);
		return;
	}
	in_meta = (RawVideoMedia::Frame *)mbuf_ancillary_data_get_buffer(
		ancillaryData, NULL);
	out_meta = *in_meta;
	out_meta.scalerOutputTimestamp = pdraw_getTimestampFromMbufFrame(
		out_frame, VSCALE_ANCILLARY_KEY_OUTPUT_TIME);
	mbuf_ancillary_data_unref(ancillaryData);
	ancillaryData = NULL;

	ret = mbuf_raw_video_frame_remove_ancillary_data(
		out_frame, PDRAW_ANCILLARY_DATA_KEY_RAWVIDEOFRAME);
	if (ret < 0) {
		self->Sink::unlock();
		PDRAW_LOG_ERRNO("mbuf_raw_video_frame_remove_ancillary_data",
				-ret);
		return;
	}

	ret = mbuf_raw_video_frame_add_ancillary_buffer(
		out_frame,
		PDRAW_ANCILLARY_DATA_KEY_RAWVIDEOFRAME,
		&out_meta,
		sizeof(out_meta));
	if (ret < 0) {
		self->Sink::unlock();
		PDRAW_LOG_ERRNO("mbuf_raw_video_frame_add_ancillary_buffer",
				-ret);
		return;
	}

	self->Sink::unlock();
	self->Source::lock();

	if (self->mOutputMedia == nullptr) {
		ret = self->createOutputMedia(&info, out_meta);
		if (ret < 0) {
			self->Source::unlock();
			PDRAW_LOG_ERRNO("createOutputMedia", -ret);
			return;
		}
	}

	/* Push the frame (unless it is silent) */
	if (!(info.info.flags & VDEF_FRAME_FLAG_SILENT)) {
		outputChannelCount =
			self->getOutputChannelCount(self->mOutputMedia);
		for (i = 0; i < outputChannelCount; i++) {
			Channel *c =
				self->getOutputChannel(self->mOutputMedia, i);
			RawVideoChannel *channel =
				dynamic_cast<RawVideoChannel *>(c);
			if (channel == nullptr) {
				PDRAW_LOGE("failed to get channel at index %d",
					   i);
				continue;
			}
			ret = channel->queue(out_frame);
			if (ret < 0)
				PDRAW_LOG_ERRNO("channel->queue", -ret);
		}
	} else {
		PDRAW_LOGD("silent frame (ignored)");
	}

	self->Source::unlock();
}


void VideoScaler::flushCb(struct vscale_scaler *scaler, void *userdata)
{
	VideoScaler *self = (VideoScaler *)userdata;

	if (userdata == nullptr) {
		PDRAW_LOG_ERRNO("userdata", EINVAL);
		return;
	}

	PDRAW_LOGD("scaler is flushed");
	self->mVscaleFlushPending = false;

	self->completeFlush();
}


void VideoScaler::stopCb(struct vscale_scaler *scaler, void *userdata)
{
	VideoScaler *self = (VideoScaler *)userdata;

	if (userdata == nullptr) {
		PDRAW_LOG_ERRNO("userdata", EINVAL);
		return;
	}

	PDRAW_LOGD("scaler is stopped");
	self->mVscaleStopPending = false;
	self->completeStop();
}

} /* namespace Pdraw */
