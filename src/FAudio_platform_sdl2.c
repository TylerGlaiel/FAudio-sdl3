/* FAudio - XAudio Reimplementation for FNA
 *
 * Copyright (c) 2011-2024 Ethan Lee, Luigi Auriemma, and the MonoGame Team
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from
 * the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 * claim that you wrote the original software. If you use this software in a
 * product, an acknowledgment in the product documentation would be
 * appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must not be
 * misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * Ethan "flibitijibibo" Lee <flibitijibibo@flibitijibibo.com>
 *
 */

#ifndef FAUDIO_WIN32_PLATFORM

#include "FAudio_internal.h"

#include <SDL3/SDL.h>

#if !SDL_VERSION_ATLEAST(2, 24, 0)
#error "SDL version older than 2.24.0"
#endif /* !SDL_VERSION_ATLEAST */

/* Mixer Thread */

static void FAudio_INTERNAL_MixCallback(void *userdata, Uint8 *stream, int len)
{
	FAudio *audio = (FAudio*) userdata;

	FAudio_zero(stream, len);
	if (audio->active)
	{
		FAudio_INTERNAL_UpdateEngine(
			audio,
			(float*) stream
		);
	}
}

//taken+modified from SDL3 migration guide
void SDLCALL MyNewAudioCallback(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount) {
	/* Calculate a little more audio here, maybe using `userdata`, write it to `stream`
	 *
	 * If you want to use the original callback, you could do something like this:
	 */
	FAudio* audio = (FAudio*)userdata;
	int update_chunk_size = audio->updateSize * sizeof(float) * audio->master->master.inputChannels;

	while(additional_amount > 0) {
		Uint8* data = SDL_stack_alloc(Uint8, update_chunk_size);
		if(data) {
			FAudio_INTERNAL_MixCallback(userdata, data, update_chunk_size);
			SDL_PutAudioStreamData(stream, data, update_chunk_size);
			SDL_stack_free(data);
		}
		additional_amount -= update_chunk_size;
	}
}

/* Platform Functions */

static void FAudio_INTERNAL_PrioritizeDirectSound()
{
	int numdrivers, i, wasapi, directsound;
	void *dll, *proc;

	if (SDL_GetHint("SDL_AUDIODRIVER") != NULL)
	{
		/* Already forced to something, ignore */
		return;
	}

	/* Windows 10+ decided to break version detection, so instead of doing
	 * it the right way we have to do something dumb like search for an
	 * export that's only in Windows 10 or newer.
	 * -flibit
	 */
	if (SDL_strcmp(SDL_GetPlatform(), "Windows") != 0)
	{
		return;
	}
	dll = SDL_LoadObject("USER32.DLL");
	if (dll == NULL)
	{
		return;
	}
	proc = SDL_LoadFunction(dll, "SetProcessDpiAwarenessContext");
	SDL_UnloadObject(dll); /* We aren't really using this, unload now */
	if (proc != NULL)
	{
		/* OS is new enough to trust WASAPI, bail */
		return;
	}

	/* Check to see if we have both Windows drivers in the list */
	numdrivers = SDL_GetNumAudioDrivers();
	wasapi = -1;
	directsound = -1;
	for (i = 0; i < numdrivers; i += 1)
	{
		const char *driver = SDL_GetAudioDriver(i);
		if (SDL_strcmp(driver, "wasapi") == 0)
		{
			wasapi = i;
		}
		else if (SDL_strcmp(driver, "directsound") == 0)
		{
			directsound = i;
		}
	}

	/* We force if and only if both drivers exist and wasapi is earlier */
	if ((wasapi > -1) && (directsound > -1))
	{
		if (wasapi < directsound)
		{
			SDL_SetHint("SDL_AUDIODRIVER", "directsound");
		}
	}
}

void FAudio_PlatformAddRef()
{
	FAudio_INTERNAL_PrioritizeDirectSound();

	/* SDL tracks ref counts for each subsystem */
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
	{
		SDL_Log("SDL_INIT_AUDIO failed: %s", SDL_GetError());
	}
	FAudio_INTERNAL_InitSIMDFunctions(
		SDL_HasSSE2(),
		SDL_HasNEON()
	);
}

void FAudio_PlatformRelease()
{
	/* SDL tracks ref counts for each subsystem */
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void FAudio_PlatformInit(
	FAudio *audio,
	uint32_t flags,
	uint32_t deviceIndex,
	FAudioWaveFormatExtensible *mixFormat,
	uint32_t *updateSize,
	void** platformDevice
) {
	SDL_AudioDeviceID device;
	SDL_AudioStream* stream;
	SDL_AudioSpec want/*, have*/;
	int wantsamples;

	FAudio_assert(mixFormat != NULL);
	FAudio_assert(updateSize != NULL);

	/* Build the device spec */
	want.freq = mixFormat->Format.nSamplesPerSec;
	want.format = SDL_AUDIO_F32;
	want.channels = mixFormat->Format.nChannels;

	if (flags & FAUDIO_1024_QUANTUM)
	{
		/* Get the sample count for a 21.33ms frame.
		 * For 48KHz this should be 1024.
		 */
		wantsamples = (int) (
			want.freq / (1000.0 / (64.0 / 3.0))
		);
	}
	else
	{
		wantsamples = want.freq / 100;
	}

	/* Open the device (or at least try to) */
iosretry:
	/*device = SDL_OpenAudioDevice(
		deviceIndex > 0 ? SDL_GetAudioDeviceName(deviceIndex - 1, 0) : NULL,
		0,
		&want,
		&have,
		0
	);*/

	stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_OUTPUT, &want, MyNewAudioCallback, audio);

	if (!stream)
	{
		const char *err = SDL_GetError();
		SDL_Log("OpenAudioDevice failed: %s", err);

		/* iOS has a weird thing where you can't open a stream when the
		 * app is in the background, even though the program is meant
		 * to be suspended and thus not trip this in the first place.
		 *
		 * Startup suspend behavior when an app is opened then closed
		 * is a big pile of crap, basically.
		 *
		 * Google the error code and you'll find that this has been a
		 * long-standing issue that nobody seems to care about.
		 * -flibit
		 */
		if (SDL_strstr(err, "Code=561015905") != NULL)
		{
			goto iosretry;
		}

		FAudio_assert(0 && "Failed to open audio device!");
		return;
	}

	/* Write up the received format for the engine */
	WriteWaveFormatExtensible(
		mixFormat,
		want.channels,
		want.freq,
		&DATAFORMAT_SUBTYPE_IEEE_FLOAT
	);
	*updateSize = wantsamples;

	*platformDevice = stream;

	/* Start the thread! */
	SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(stream));
}

void FAudio_PlatformQuit(void* platformDevice)
{
	SDL_DestroyAudioStream((SDL_AudioStream*)platformDevice);
}

uint32_t FAudio_PlatformGetDeviceCount()
{
	int num_devices;
	SDL_AudioDeviceID* devices = SDL_GetAudioOutputDevices(&num_devices);
	if (!devices)
	{
		return 0;
	}
	SDL_free(devices);
	return num_devices + 1; /* Add one for "Default Device" */
}

void FAudio_UTF8_To_UTF16(const char *src, uint16_t *dst, size_t len);

uint32_t FAudio_PlatformGetDeviceDetails(
	uint32_t index,
	FAudioDeviceDetails *details
) {
	const char *name, *envvar;
	int channels, rate;
	SDL_AudioSpec spec;
	uint32_t devcount;

	FAudio_zero(details, sizeof(FAudioDeviceDetails));

	devcount = FAudio_PlatformGetDeviceCount();
	if (index >= devcount)
	{
		return FAUDIO_E_INVALID_CALL;
	}

	details->DeviceID[0] = L'0' + index;
	if (index == 0)
	{
		name = "Default Device";
		details->Role = FAudioGlobalDefaultDevice;

		/* This variable will look like a DSound GUID or WASAPI ID, i.e.
		 * "{0.0.0.00000000}.{FD47D9CC-4218-4135-9CE2-0C195C87405B}"
		 */
		envvar = SDL_getenv("FAUDIO_FORCE_DEFAULT_DEVICEID");
		if (envvar != NULL)
		{
			FAudio_UTF8_To_UTF16(
				envvar,
				(uint16_t*) details->DeviceID,
				sizeof(details->DeviceID)
			);
		}
	}
	else
	{
		name = SDL_GetAudioDeviceName(index - 1, 0);
		details->Role = FAudioNotDefaultDevice;
	}
	FAudio_UTF8_To_UTF16(
		name,
		(uint16_t*) details->DisplayName,
		sizeof(details->DisplayName)
	);

	/* Environment variables take precedence over all possible values */
	envvar = SDL_getenv("SDL_AUDIO_FREQUENCY");
	if (envvar != NULL)
	{
		rate = SDL_atoi(envvar);
	}
	else
	{
		rate = 0;
	}
	envvar = SDL_getenv("SDL_AUDIO_CHANNELS");
	if (envvar != NULL)
	{
		channels = SDL_atoi(envvar);
	}
	else
	{
		channels = 0;
	}

	/* Get the device format from the OS */
	if (index == 0)
	{
		/* TODO: Do we want to squeeze the name into the output? */
		if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_OUTPUT, &spec, NULL) < 0)
		{
			SDL_zero(spec);
		}
	}
	else
	{
		SDL_GetAudioDeviceFormat(index - 1, &spec, NULL);
	}
	if ((spec.freq > 0) && (rate <= 0))
	{
		rate = spec.freq;
	}
	if ((spec.channels > 0) && (spec.channels < 9) && (channels <= 0))
	{
		channels = spec.channels;
	}

	/* If we make it all the way here with no format, hardcode a sane one */
	if (rate <= 0)
	{
		rate = 48000;
	}
	if (channels <= 0)
	{
		channels = 2;
	}

	/* Write the format, finally. */
	WriteWaveFormatExtensible(
		&details->OutputFormat,
		channels,
		rate,
		&DATAFORMAT_SUBTYPE_PCM
	);
	return 0;
}

/* Threading */

FAudioThread FAudio_PlatformCreateThread(
	FAudioThreadFunc func,
	const char *name,
	void* data
) {
	return (FAudioThread) SDL_CreateThread(
		(SDL_ThreadFunction) func,
		name,
		data
	);
}

void FAudio_PlatformWaitThread(FAudioThread thread, int32_t *retval)
{
	SDL_WaitThread((SDL_Thread*) thread, retval);
}

void FAudio_PlatformThreadPriority(FAudioThreadPriority priority)
{
	SDL_SetThreadPriority((SDL_ThreadPriority) priority);
}

uint64_t FAudio_PlatformGetThreadID(void)
{
	return (uint64_t) SDL_GetCurrentThreadID();
}

FAudioMutex FAudio_PlatformCreateMutex()
{
	return (FAudioMutex) SDL_CreateMutex();
}

void FAudio_PlatformDestroyMutex(FAudioMutex mutex)
{
	SDL_DestroyMutex((SDL_Mutex*) mutex);
}

void FAudio_PlatformLockMutex(FAudioMutex mutex)
{
	SDL_LockMutex((SDL_Mutex*) mutex);
}

void FAudio_PlatformUnlockMutex(FAudioMutex mutex)
{
	SDL_UnlockMutex((SDL_Mutex*) mutex);
}

void FAudio_sleep(uint32_t ms)
{
	SDL_Delay(ms);
}

/* Time */

uint32_t FAudio_timems()
{
	return SDL_GetTicks();
}

/* FAudio I/O */

//copied from SDL3 migration guide
static size_t SDL_ReadIOAdapter(SDL_IOStream* stream, void* ptr, size_t size, size_t nitems) {
	if(size > 0 && nitems > 0) {
		return SDL_ReadIO(stream, ptr, size * nitems) / size;
	}
	return 0;
}


FAudioIOStream* FAudio_fopen(const char *path)
{
	FAudioIOStream *io = (FAudioIOStream*) FAudio_malloc(
		sizeof(FAudioIOStream)
	);
	SDL_IOStream *rwops = SDL_IOFromFile(path, "rb");

	io->data = rwops;
	io->read = (FAudio_readfunc)SDL_ReadIOAdapter;
	io->seek = (FAudio_seekfunc) SDL_SeekIO;
	io->close = (FAudio_closefunc) SDL_CloseIO;
	io->lock = FAudio_PlatformCreateMutex();
	return io;
}

FAudioIOStream* FAudio_memopen(void *mem, int len)
{
	FAudioIOStream *io = (FAudioIOStream*) FAudio_malloc(
		sizeof(FAudioIOStream)
	);
	SDL_IOStream *rwops = SDL_IOFromMem(mem, len);
	io->data = rwops;
	io->read = (FAudio_readfunc)SDL_ReadIOAdapter;
	io->seek = (FAudio_seekfunc)SDL_SeekIO;
	io->close = (FAudio_closefunc)SDL_CloseIO;
	io->lock = FAudio_PlatformCreateMutex();
	return io;
}

uint8_t* FAudio_memptr(FAudioIOStream *io, size_t offset)
{
	return NULL; //TODO
	/*SDL_IOStream *rwops = (SDL_IOStream*) io->data;
	FAudio_assert(rwops->type == SDL_RWOPS_MEMORY);
	return rwops->hidden.mem.base + offset;*/
}

void FAudio_close(FAudioIOStream *io)
{
	io->close(io->data);
	FAudio_PlatformDestroyMutex((FAudioMutex) io->lock);
	FAudio_free(io);
}

#ifdef FAUDIO_DUMP_VOICES
FAudioIOStreamOut* FAudio_fopen_out(const char *path, const char *mode)
{
	FAudioIOStreamOut *io = (FAudioIOStreamOut*) FAudio_malloc(
		sizeof(FAudioIOStreamOut)
	);
	SDL_RWops *rwops = SDL_RWFromFile(path, mode);
	io->data = rwops;
	io->read = (FAudio_readfunc) rwops->read;
	io->write = (FAudio_writefunc) rwops->write;
	io->seek = (FAudio_seekfunc) rwops->seek;
	io->size = (FAudio_sizefunc) rwops->size;
	io->close = (FAudio_closefunc) rwops->close;
	io->lock = FAudio_PlatformCreateMutex();
	return io;
}

void FAudio_close_out(FAudioIOStreamOut *io)
{
	io->close(io->data);
	FAudio_PlatformDestroyMutex((FAudioMutex) io->lock);
	FAudio_free(io);
}
#endif /* FAUDIO_DUMP_VOICES */

/* UTF8->UTF16 Conversion, taken from PhysicsFS */

#define UNICODE_BOGUS_CHAR_VALUE 0xFFFFFFFF
#define UNICODE_BOGUS_CHAR_CODEPOINT '?'

static uint32_t FAudio_UTF8_CodePoint(const char **_str)
{
    const char *str = *_str;
    uint32_t retval = 0;
    uint32_t octet = (uint32_t) ((uint8_t) *str);
    uint32_t octet2, octet3, octet4;

    if (octet == 0)  /* null terminator, end of string. */
        return 0;

    else if (octet < 128)  /* one octet char: 0 to 127 */
    {
        (*_str)++;  /* skip to next possible start of codepoint. */
        return octet;
    } /* else if */

    else if ((octet > 127) && (octet < 192))  /* bad (starts with 10xxxxxx). */
    {
        /*
         * Apparently each of these is supposed to be flagged as a bogus
         *  char, instead of just resyncing to the next valid codepoint.
         */
        (*_str)++;  /* skip to next possible start of codepoint. */
        return UNICODE_BOGUS_CHAR_VALUE;
    } /* else if */

    else if (octet < 224)  /* two octets */
    {
        (*_str)++;  /* advance at least one byte in case of an error */
        octet -= (128+64);
        octet2 = (uint32_t) ((uint8_t) *(++str));
        if ((octet2 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        *_str += 1;  /* skip to next possible start of codepoint. */
        retval = ((octet << 6) | (octet2 - 128));
        if ((retval >= 0x80) && (retval <= 0x7FF))
            return retval;
    } /* else if */

    else if (octet < 240)  /* three octets */
    {
        (*_str)++;  /* advance at least one byte in case of an error */
        octet -= (128+64+32);
        octet2 = (uint32_t) ((uint8_t) *(++str));
        if ((octet2 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet3 = (uint32_t) ((uint8_t) *(++str));
        if ((octet3 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        *_str += 2;  /* skip to next possible start of codepoint. */
        retval = ( ((octet << 12)) | ((octet2-128) << 6) | ((octet3-128)) );

        /* There are seven "UTF-16 surrogates" that are illegal in UTF-8. */
        switch (retval)
        {
            case 0xD800:
            case 0xDB7F:
            case 0xDB80:
            case 0xDBFF:
            case 0xDC00:
            case 0xDF80:
            case 0xDFFF:
                return UNICODE_BOGUS_CHAR_VALUE;
        } /* switch */

        /* 0xFFFE and 0xFFFF are illegal, too, so we check them at the edge. */
        if ((retval >= 0x800) && (retval <= 0xFFFD))
            return retval;
    } /* else if */

    else if (octet < 248)  /* four octets */
    {
        (*_str)++;  /* advance at least one byte in case of an error */
        octet -= (128+64+32+16);
        octet2 = (uint32_t) ((uint8_t) *(++str));
        if ((octet2 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet3 = (uint32_t) ((uint8_t) *(++str));
        if ((octet3 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet4 = (uint32_t) ((uint8_t) *(++str));
        if ((octet4 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        *_str += 3;  /* skip to next possible start of codepoint. */
        retval = ( ((octet << 18)) | ((octet2 - 128) << 12) |
                   ((octet3 - 128) << 6) | ((octet4 - 128)) );
        if ((retval >= 0x10000) && (retval <= 0x10FFFF))
            return retval;
    } /* else if */

    /*
     * Five and six octet sequences became illegal in rfc3629.
     *  We throw the codepoint away, but parse them to make sure we move
     *  ahead the right number of bytes and don't overflow the buffer.
     */

    else if (octet < 252)  /* five octets */
    {
        (*_str)++;  /* advance at least one byte in case of an error */
        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        *_str += 4;  /* skip to next possible start of codepoint. */
        return UNICODE_BOGUS_CHAR_VALUE;
    } /* else if */

    else  /* six octets */
    {
        (*_str)++;  /* advance at least one byte in case of an error */
        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        *_str += 6;  /* skip to next possible start of codepoint. */
        return UNICODE_BOGUS_CHAR_VALUE;
    } /* else if */

    return UNICODE_BOGUS_CHAR_VALUE;
}

void FAudio_UTF8_To_UTF16(const char *src, uint16_t *dst, size_t len)
{
    len -= sizeof (uint16_t);   /* save room for null char. */
    while (len >= sizeof (uint16_t))
    {
        uint32_t cp = FAudio_UTF8_CodePoint(&src);
        if (cp == 0)
            break;
        else if (cp == UNICODE_BOGUS_CHAR_VALUE)
            cp = UNICODE_BOGUS_CHAR_CODEPOINT;

        if (cp > 0xFFFF)  /* encode as surrogate pair */
        {
            if (len < (sizeof (uint16_t) * 2))
                break;  /* not enough room for the pair, stop now. */

            cp -= 0x10000;  /* Make this a 20-bit value */

            *(dst++) = 0xD800 + ((cp >> 10) & 0x3FF);
            len -= sizeof (uint16_t);

            cp = 0xDC00 + (cp & 0x3FF);
        } /* if */

        *(dst++) = cp;
        len -= sizeof (uint16_t);
    } /* while */

    *dst = 0;
}

/* vim: set noexpandtab shiftwidth=8 tabstop=8: */

#else

extern int this_tu_is_empty;

#endif /* FAUDIO_WIN32_PLATFORM */
