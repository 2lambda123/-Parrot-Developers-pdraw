/**
 * Parrot Drones Awesome Video Viewer Library
 * User settings
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

#ifndef _PDRAW_SETTINGS_HPP_
#define _PDRAW_SETTINGS_HPP_

#include <inttypes.h>
#include <math.h>
#include <pthread.h>

#include <pdraw/pdraw_defs.h>

#include <string>

namespace Pdraw {


#define SETTINGS_DISPLAY_XDPI (200.0f)
#define SETTINGS_DISPLAY_YDPI (200.0f)
#define SETTINGS_DISPLAY_DEVICE_MARGIN (0.0f)


class Settings {
public:
	Settings(void);

	~Settings(void);

	void lock(void);

	void unlock(void);

	void getFriendlyName(std::string *friendlyName);

	void setFriendlyName(const std::string &friendlyName);

	void getSerialNumber(std::string *serialNumber);

	void setSerialNumber(const std::string &serialNumber);

	void getSoftwareVersion(std::string *softwareVersion);

	void setSoftwareVersion(const std::string &softwareVersion);

	enum pdraw_pipeline_mode getPipelineMode(void);

	void setPipelineMode(enum pdraw_pipeline_mode mode);

	void getDisplayScreenSettings(float *xdpi,
				      float *ydpi,
				      float *deviceMarginTop,
				      float *deviceMarginBottom,
				      float *deviceMarginLeft,
				      float *deviceMarginRight);

	void setDisplayScreenSettings(float xdpi,
				      float ydpi,
				      float deviceMarginTop,
				      float deviceMarginBottom,
				      float deviceMarginLeft,
				      float deviceMarginRight);

	enum pdraw_hmd_model getHmdModelSetting(void);

	void setHmdModelSetting(enum pdraw_hmd_model hmdModel);

private:
	pthread_mutex_t mMutex;
	std::string mFriendlyName;
	std::string mSerialNumber;
	std::string mSoftwareVersion;
	enum pdraw_pipeline_mode mPipelineMode;
	float mDisplayXdpi;
	float mDisplayYdpi;
	float mDisplayDeviceMarginTop;
	float mDisplayDeviceMarginBottom;
	float mDisplayDeviceMarginLeft;
	float mDisplayDeviceMarginRight;
	enum pdraw_hmd_model mHmdModel;
};

} /* namespace Pdraw */

#endif /* !_PDRAW_SETTINGS_HPP_ */
