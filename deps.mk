pickle.o: pickle.c drm_keystone.h keystone.h drm.h pickle_globals.h egl.h \
 v4l2_player.h v4l2_decoder.h utils.h shader.h input.h hvs_keystone.h \
 compute_keystone.h
utils.o: utils.c utils.h
shader.o: shader.c shader.h utils.h
keystone.o: keystone.c keystone.h utils.h shader.h hvs_keystone.h \
 drm_keystone.h drm.h log.h
keystone_funcs.o: keystone_funcs.c keystone.h utils.h
drm.o: drm.c drm.h utils.h
drm_atomic.o: drm_atomic.c drm.h log.h
drm_keystone.o: drm_keystone.c drm_keystone.h keystone.h drm.h log.h \
 drm_atomic.h /usr/include/libdrm/drm_fourcc.h
egl.o: egl.c egl.h drm.h utils.h keystone.h log.h
egl_dmabuf.o: egl_dmabuf.c /usr/include/libdrm/drm_fourcc.h \
 /usr/include/libdrm/drm.h /usr/include/libdrm/drm_mode.h drm.h egl.h \
 log.h
render_video.o: render_video.c egl.h drm.h shader.h
zero_copy.o: zero_copy.c /usr/include/libdrm/drm_fourcc.h \
 /usr/include/libdrm/drm.h /usr/include/libdrm/drm_mode.h drm.h egl.h \
 log.h
input.o: input.c input.h utils.h keystone.h
error.o: error.c error.h
frame_pacing.o: frame_pacing.c frame_pacing.h error.h utils.h
render.o: render.c
mpv.o: mpv.c mpv.h error.h utils.h
dispmanx.o: dispmanx.c dispmanx.h log.h egl.h drm.h
v4l2_decoder.o: v4l2_decoder.c v4l2_decoder.h log.h
hvs_keystone.o: hvs_keystone.c hvs_keystone.h keystone.h dispmanx.h log.h
compute_keystone.o: compute_keystone.c compute_keystone.h keystone.h \
 shader.h utils.h log.h
event.o: event.c event.h
event_callbacks.o: event_callbacks.c event_callbacks.h event.h mpv.h \
 error.h keystone.h drm.h v4l2_player.h v4l2_decoder.h pickle_globals.h \
 egl.h input.h
pickle_events.o: pickle_events.c pickle_events.h event.h mpv.h error.h \
 drm.h v4l2_decoder.h v4l2_player.h event_callbacks.h keystone.h \
 pickle_globals.h egl.h input.h
pickle_globals.o: pickle_globals.c pickle_globals.h drm.h egl.h \
 v4l2_player.h v4l2_decoder.h
mpv_render.o: mpv_render.c pickle_globals.h drm.h egl.h v4l2_player.h \
 v4l2_decoder.h mpv.h error.h
