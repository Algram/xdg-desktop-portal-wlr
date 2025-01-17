#include "pipewire_screencast.h"

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>
#include <spa/param/props.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <sys/mman.h>
#include <unistd.h>
#include <assert.h>
#include <libdrm/drm_fourcc.h>

#include "wlr_screencast.h"
#include "xdpw.h"
#include "logger.h"

static struct spa_pod *build_buffer(struct spa_pod_builder *b, uint32_t blocks, uint32_t size,
		uint32_t stride, uint32_t datatype) {
	assert(blocks > 0);
	assert(datatype > 0);
	struct spa_pod_frame f[1];

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
	spa_pod_builder_add(b, SPA_PARAM_BUFFERS_buffers,
			SPA_POD_CHOICE_RANGE_Int(XDPW_PWR_BUFFERS, XDPW_PWR_BUFFERS_MIN, 32), 0);
	spa_pod_builder_add(b, SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(blocks), 0);
	if (size > 0) {
		spa_pod_builder_add(b, SPA_PARAM_BUFFERS_size, SPA_POD_Int(size), 0);
	}
	if (stride > 0) {
		spa_pod_builder_add(b, SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride), 0);
	}
	spa_pod_builder_add(b, SPA_PARAM_BUFFERS_align, SPA_POD_Int(XDPW_PWR_ALIGN), 0);
	spa_pod_builder_add(b, SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(datatype), 0);
	return spa_pod_builder_pop(b, &f[0]);
}

static struct spa_pod *build_format(struct spa_pod_builder *b, enum spa_video_format format,
		uint32_t width, uint32_t height, uint32_t framerate,
		uint64_t *modifiers, int modifier_count) {
	struct spa_pod_frame f[2];
	int i, c;

	enum spa_video_format format_without_alpha = xdpw_format_pw_strip_alpha(format);

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
	spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
	/* format */
	if (modifier_count > 0 || format_without_alpha == SPA_VIDEO_FORMAT_UNKNOWN) {
		// modifiers are defined only in combinations with their format
		// we should not announce the format without alpha
		spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
	} else {
		spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format,
				SPA_POD_CHOICE_ENUM_Id(3, format, format, format_without_alpha), 0);
	}
	/* modifiers */
	if (modifier_count == 1 && modifiers[0] == DRM_FORMAT_MOD_INVALID) {
		// we only support implicit modifiers, use shortpath to skip fixation phase
		spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
		spa_pod_builder_long(b, modifiers[0]);
	} else if (modifier_count > 0) {
		// build an enumeration of modifiers
		spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
		spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
		// modifiers from the array
		for (i = 0, c = 0; i < modifier_count; i++) {
			spa_pod_builder_long(b, modifiers[i]);
			if (c++ == 0)
				spa_pod_builder_long(b, modifiers[i]);
		}
		spa_pod_builder_pop(b, &f[1]);
	}
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
		SPA_POD_Rectangle(&SPA_RECTANGLE(width, height)),
		0);
	// variable framerate
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate,
		SPA_POD_Fraction(&SPA_FRACTION(0, 1)), 0);
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_maxFramerate,
		SPA_POD_CHOICE_RANGE_Fraction(
			&SPA_FRACTION(framerate, 1),
			&SPA_FRACTION(1, 1),
			&SPA_FRACTION(framerate, 1)),
		0);
	return spa_pod_builder_pop(b, &f[0]);
}

static uint32_t build_formats(struct spa_pod_builder *b, struct xdpw_screencast_instance *cast,
		const struct spa_pod *params[static 2]) {
	uint32_t param_count;
	uint32_t modifier_count = 1;
	uint64_t modifier = DRM_FORMAT_MOD_INVALID;

	if (cast->ctx->gbm) {
		param_count = 2;
		params[0] = build_format(b, xdpw_format_pw_from_drm_fourcc(cast->screencopy_frame_info[DMABUF].format),
				cast->screencopy_frame_info[DMABUF].width, cast->screencopy_frame_info[DMABUF].height, cast->framerate,
				&modifier, modifier_count);
		params[1] = build_format(b, xdpw_format_pw_from_drm_fourcc(cast->screencopy_frame_info[WL_SHM].format),
				cast->screencopy_frame_info[WL_SHM].width, cast->screencopy_frame_info[WL_SHM].height, cast->framerate,
				NULL, 0);
	} else {
		param_count = 1;
		params[0] = build_format(b, xdpw_format_pw_from_drm_fourcc(cast->screencopy_frame_info[WL_SHM].format),
				cast->screencopy_frame_info[WL_SHM].width, cast->screencopy_frame_info[WL_SHM].height, cast->framerate,
				NULL, 0);
	}

	return param_count;
}

static void pwr_handle_stream_on_process(void *data) {
	logprint(TRACE, "pipewire: stream process");
	struct xdpw_screencast_instance *cast = data;

	if (cast->need_buffer) {
		xdpw_pwr_dequeue_buffer(cast);
		if (cast->current_frame.pw_buffer) {
			cast->need_buffer = false;
		}
	}
}

static void pwr_handle_stream_state_changed(void *data,
		enum pw_stream_state old, enum pw_stream_state state, const char *error) {
	struct xdpw_screencast_instance *cast = data;
	cast->node_id = pw_stream_get_node_id(cast->stream);

	logprint(INFO, "pipewire: stream state changed to \"%s\"",
		pw_stream_state_as_string(state));
	logprint(INFO, "pipewire: node id is %d", (int)cast->node_id);

	switch (state) {
	case PW_STREAM_STATE_STREAMING:
		cast->pwr_stream_state = true;
		if (cast->frame_state == XDPW_FRAME_STATE_NONE) {
			xdpw_wlr_frame_start(cast);
		}
		break;
	case PW_STREAM_STATE_PAUSED:
		if (old == PW_STREAM_STATE_STREAMING) {
			xdpw_pwr_enqueue_buffer(cast);
		}
		// fall through
	default:
		cast->pwr_stream_state = false;
		break;
	}
}

static void pwr_handle_stream_param_changed(void *data, uint32_t id,
		const struct spa_pod *param) {
	logprint(TRACE, "pipewire: stream parameters changed");
	struct xdpw_screencast_instance *cast = data;
	struct pw_stream *stream = cast->stream;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b =
		SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[2];
	uint32_t blocks;
	uint32_t data_type;

	if (!param || id != SPA_PARAM_Format) {
		return;
	}

	spa_format_video_raw_parse(param, &cast->pwr_format);
	cast->framerate = (uint32_t)(cast->pwr_format.max_framerate.num / cast->pwr_format.max_framerate.denom);

	if (spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier) != NULL) {
		if (cast->pwr_format.modifier != DRM_FORMAT_MOD_INVALID) {
			abort();
		}
		cast->buffer_type = DMABUF;
		blocks = 1;
		data_type = 1<<SPA_DATA_DmaBuf;
	} else {
		cast->buffer_type = WL_SHM;
		blocks = 1;
		data_type = 1<<SPA_DATA_MemFd;
	}

	logprint(DEBUG, "pipewire: Format negotiated:");
	logprint(DEBUG, "pipewire: buffer_type: %u (%u)", cast->buffer_type, data_type);
	logprint(DEBUG, "pipewire: format: %u", cast->pwr_format.format);
	logprint(DEBUG, "pipewire: modifier: %lu", cast->pwr_format.modifier);
	logprint(DEBUG, "pipewire: size: (%u, %u)", cast->pwr_format.size.width, cast->pwr_format.size.height);
	logprint(DEBUG, "pipewire: max_framerate: (%u / %u)", cast->pwr_format.max_framerate.num, cast->pwr_format.max_framerate.denom);

	params[0] = build_buffer(&b, blocks, cast->screencopy_frame_info[cast->buffer_type].size,
			cast->screencopy_frame_info[cast->buffer_type].stride, data_type);

	params[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

	pw_stream_update_params(stream, params, 2);
}

static void pwr_handle_stream_add_buffer(void *data, struct pw_buffer *buffer) {
	struct xdpw_screencast_instance *cast = data;
	struct spa_data *d;

	logprint(DEBUG, "pipewire: add buffer event handle");

	d = buffer->buffer->datas;

	// Select buffer type from negotiation result
	if ((d[0].type & (1u << SPA_DATA_MemFd)) > 0) {
		assert(cast->buffer_type == WL_SHM);
		d[0].type = SPA_DATA_MemFd;
	} else if ((d[0].type & (1u << SPA_DATA_DmaBuf)) > 0) {
		assert(cast->buffer_type == DMABUF);
		d[0].type = SPA_DATA_DmaBuf;
	} else {
		logprint(ERROR, "pipewire: unsupported buffer type");
		cast->err = 1;
		return;
	}

	logprint(TRACE, "pipewire: selected buffertype %u", d[0].type);

	struct xdpw_buffer *xdpw_buffer = xdpw_buffer_create(cast, cast->buffer_type, &cast->screencopy_frame_info[cast->buffer_type]);
	if (xdpw_buffer == NULL) {
		logprint(ERROR, "pipewire: failed to create xdpw buffer");
		cast->err = 1;
		return;
	}
	wl_list_insert(&cast->buffer_list, &xdpw_buffer->link);
	buffer->user_data = xdpw_buffer;

	d[0].maxsize = xdpw_buffer->size;
	d[0].mapoffset = 0;
	d[0].chunk->size = xdpw_buffer->size;
	d[0].chunk->stride = xdpw_buffer->stride;
	d[0].chunk->offset = xdpw_buffer->offset;
	d[0].flags = 0;
	d[0].fd = xdpw_buffer->fd;
	d[0].data = NULL;

	// clients have implemented to check chunk->size if the buffer is valid instead
	// of using the flags. Until they are patched we should use some arbitrary value.
	if (xdpw_buffer->buffer_type == DMABUF && d[0].chunk->size == 0) {
		d[0].chunk->size = 9; // This was choosen by a fair d20.
	}
}

static void pwr_handle_stream_remove_buffer(void *data, struct pw_buffer *buffer) {
	struct xdpw_screencast_instance *cast = data;

	logprint(DEBUG, "pipewire: remove buffer event handle");

	struct xdpw_buffer *xdpw_buffer = buffer->user_data;
	if (xdpw_buffer) {
		xdpw_buffer_destroy(xdpw_buffer);
	}
	if (cast->current_frame.pw_buffer == buffer) {
		cast->current_frame.pw_buffer = NULL;
	}
	buffer->buffer->datas[0].fd = -1;
	buffer->user_data = NULL;
}

static const struct pw_stream_events pwr_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = pwr_handle_stream_state_changed,
	.param_changed = pwr_handle_stream_param_changed,
	.add_buffer = pwr_handle_stream_add_buffer,
	.remove_buffer = pwr_handle_stream_remove_buffer,
	.process = pwr_handle_stream_on_process,
};

void xdpw_pwr_dequeue_buffer(struct xdpw_screencast_instance *cast) {
	logprint(TRACE, "pipewire: dequeueing buffer");

	assert(!cast->current_frame.pw_buffer);
	if ((cast->current_frame.pw_buffer = pw_stream_dequeue_buffer(cast->stream)) == NULL) {
		logprint(WARN, "pipewire: out of buffers");
		return;
	}

	cast->current_frame.xdpw_buffer = cast->current_frame.pw_buffer->user_data;
}

void xdpw_pwr_enqueue_buffer(struct xdpw_screencast_instance *cast) {
	logprint(TRACE, "pipewire: enqueueing buffer");

	if (!cast->current_frame.pw_buffer) {
		logprint(WARN, "pipewire: no buffer to queue");
		goto done;
	}
	struct pw_buffer *pw_buf = cast->current_frame.pw_buffer;
	struct spa_buffer *spa_buf = pw_buf->buffer;
	struct spa_data *d = spa_buf->datas;

	bool buffer_corrupt = cast->frame_state != XDPW_FRAME_STATE_SUCCESS;

	if (cast->current_frame.y_invert) {
		//TODO: Flip buffer or set stride negative
		buffer_corrupt = true;
		cast->err = 1;
	}

	struct spa_meta_header *h;
	if ((h = spa_buffer_find_meta_data(spa_buf, SPA_META_Header, sizeof(*h)))) {
		h->pts = -1;
		h->flags = buffer_corrupt ? SPA_META_HEADER_FLAG_CORRUPTED : 0;
		h->seq = cast->seq++;
		h->dts_offset = 0;
	}

	if (buffer_corrupt) {
		d[0].chunk->flags = SPA_CHUNK_FLAG_CORRUPTED;
	} else {
		d[0].chunk->flags = SPA_CHUNK_FLAG_NONE;
	}

	logprint(TRACE, "********************");
	logprint(TRACE, "pipewire: fd %u", d[0].fd);
	logprint(TRACE, "pipewire: maxsize %d", d[0].maxsize);
	logprint(TRACE, "pipewire: size %d", d[0].chunk->size);
	logprint(TRACE, "pipewire: stride %d", d[0].chunk->stride);
	logprint(TRACE, "pipewire: offset %d", d[0].chunk->offset);
	logprint(TRACE, "pipewire: chunk flags %d", d[0].chunk->flags);
	logprint(TRACE, "pipewire: width %d", cast->current_frame.xdpw_buffer->width);
	logprint(TRACE, "pipewire: height %d", cast->current_frame.xdpw_buffer->height);
	logprint(TRACE, "pipewire: y_invert %d", cast->current_frame.y_invert);
	logprint(TRACE, "********************");

	pw_stream_queue_buffer(cast->stream, pw_buf);

done:
	cast->current_frame.xdpw_buffer = NULL;
	cast->current_frame.pw_buffer = NULL;
}

void xdpw_pwr_swap_buffer(struct xdpw_screencast_instance *cast) {
	logprint(TRACE, "pipewire: swapping buffers");

	if (!cast->current_frame.pw_buffer) {
		goto dequeue_buffer;
	}

	xdpw_pwr_enqueue_buffer(cast);

dequeue_buffer:
	assert(!cast->current_frame.pw_buffer);
	cast->need_buffer = false;
	xdpw_pwr_dequeue_buffer(cast);
	if (!cast->current_frame.pw_buffer) {
		cast->need_buffer = true;
	}
}

void pwr_update_stream_param(struct xdpw_screencast_instance *cast) {
	logprint(TRACE, "pipewire: stream update parameters");
	struct pw_stream *stream = cast->stream;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b =
		SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[2];

	uint32_t n_params = build_formats(&b, cast, params);

	pw_stream_update_params(stream, params, n_params);
}

void xdpw_pwr_stream_create(struct xdpw_screencast_instance *cast) {
	struct xdpw_screencast_context *ctx = cast->ctx;
	struct xdpw_state *state = ctx->state;

	pw_loop_enter(state->pw_loop);

	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[2];

	char name[] = "xdpw-stream-XXXXXX";
	randname(name + strlen(name) - 6);
	cast->stream = pw_stream_new(ctx->core, name,
		pw_properties_new(
			PW_KEY_MEDIA_CLASS, "Video/Source",
			NULL));

	if (!cast->stream) {
		logprint(ERROR, "pipewire: failed to create stream");
		abort();
	}
	cast->pwr_stream_state = false;

	uint32_t param_count = build_formats(&b, cast, params);

	pw_stream_add_listener(cast->stream, &cast->stream_listener,
		&pwr_stream_events, cast);

	pw_stream_connect(cast->stream,
		PW_DIRECTION_OUTPUT,
		PW_ID_ANY,
		(PW_STREAM_FLAG_DRIVER |
			PW_STREAM_FLAG_ALLOC_BUFFERS),
		params, param_count);
}

void xdpw_pwr_stream_destroy(struct xdpw_screencast_instance *cast) {
	if (!cast->stream) {
		return;
	}

	logprint(DEBUG, "pipewire: destroying stream");
	pw_stream_flush(cast->stream, false);
	pw_stream_disconnect(cast->stream);
	pw_stream_destroy(cast->stream);
	cast->stream = NULL;
}

int xdpw_pwr_context_create(struct xdpw_state *state) {
	struct xdpw_screencast_context *ctx = &state->screencast;

	logprint(DEBUG, "pipewire: establishing connection to core");

	if (!ctx->pwr_context) {
		ctx->pwr_context = pw_context_new(state->pw_loop, NULL, 0);
		if (!ctx->pwr_context) {
			logprint(ERROR, "pipewire: failed to create context");
			return -1;
		}
	}

	if (!ctx->core) {
		ctx->core = pw_context_connect(ctx->pwr_context, NULL, 0);
		if (!ctx->core) {
			logprint(ERROR, "pipewire: couldn't connect to context");
			return -1;
		}
	}
	return 0;
}

void xdpw_pwr_context_destroy(struct xdpw_state *state) {
	struct xdpw_screencast_context *ctx = &state->screencast;

	logprint(DEBUG, "pipewire: disconnecting fom core");

	if (ctx->core) {
		pw_core_disconnect(ctx->core);
		ctx->core = NULL;
	}

	if (ctx->pwr_context) {
		pw_context_destroy(ctx->pwr_context);
		ctx->pwr_context = NULL;
	}
}
