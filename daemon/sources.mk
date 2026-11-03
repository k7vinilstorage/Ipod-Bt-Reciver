# BTstack sources needed by a2dpsinkd (paths relative to the BTstack checkout)
BTSTACK_SOURCES = \
	src/ad_parser.c \
	src/btstack_audio.c \
	src/btstack_linked_list.c \
	src/btstack_memory.c \
	src/btstack_memory_pool.c \
	src/btstack_resample.c \
	src/btstack_ring_buffer.c \
	src/btstack_run_loop.c \
	src/btstack_tlv.c \
	src/btstack_util.c \
	src/hci.c \
	src/hci_cmd.c \
	src/hci_dump.c \
	src/hci_event.c \
	src/l2cap.c \
	src/l2cap_signaling.c \
	src/classic/a2dp.c \
	src/classic/a2dp_sink.c \
	src/classic/a2dp_source.c \
	src/classic/avdtp.c \
	src/classic/avdtp_acceptor.c \
	src/classic/avdtp_initiator.c \
	src/classic/avdtp_sink.c \
	src/classic/avdtp_source.c \
	src/classic/avdtp_util.c \
	src/classic/avrcp.c \
	src/classic/avrcp_controller.c \
	src/classic/avrcp_target.c \
	src/classic/btstack_link_key_db_tlv.c \
	src/classic/btstack_sbc_bluedroid.c \
	src/classic/btstack_sbc_decoder_bluedroid.c \
	src/classic/btstack_sbc_plc.c \
	src/classic/device_id_server.c \
	src/classic/sdp_client.c \
	src/classic/sdp_server.c \
	src/classic/sdp_util.c \
	platform/posix/btstack_run_loop_posix.c \
	platform/posix/btstack_tlv_posix.c \
	platform/posix/hci_dump_posix_fs.c \
	platform/posix/hci_dump_posix_stdout.c \
	3rd-party/bluedroid/decoder/srce/alloc.c \
	3rd-party/bluedroid/decoder/srce/bitalloc.c \
	3rd-party/bluedroid/decoder/srce/bitalloc-sbc.c \
	3rd-party/bluedroid/decoder/srce/bitstream-decode.c \
	3rd-party/bluedroid/decoder/srce/decoder-oina.c \
	3rd-party/bluedroid/decoder/srce/decoder-private.c \
	3rd-party/bluedroid/decoder/srce/decoder-sbc.c \
	3rd-party/bluedroid/decoder/srce/dequant.c \
	3rd-party/bluedroid/decoder/srce/framing.c \
	3rd-party/bluedroid/decoder/srce/framing-sbc.c \
	3rd-party/bluedroid/decoder/srce/oi_codec_version.c \
	3rd-party/bluedroid/decoder/srce/synthesis-8-generated.c \
	3rd-party/bluedroid/decoder/srce/synthesis-dct8.c \
	3rd-party/bluedroid/decoder/srce/synthesis-sbc.c \
	3rd-party/bluedroid/encoder/srce/sbc_analysis.c \
	3rd-party/bluedroid/encoder/srce/sbc_dct.c \
	3rd-party/bluedroid/encoder/srce/sbc_dct_coeffs.c \
	3rd-party/bluedroid/encoder/srce/sbc_enc_bit_alloc_mono.c \
	3rd-party/bluedroid/encoder/srce/sbc_enc_bit_alloc_ste.c \
	3rd-party/bluedroid/encoder/srce/sbc_enc_coeffs.c \
	3rd-party/bluedroid/encoder/srce/sbc_encoder.c \
	3rd-party/bluedroid/encoder/srce/sbc_packing.c

BTSTACK_INCLUDES = src src/classic platform/posix 3rd-party/bluedroid/decoder/include 3rd-party/bluedroid/encoder/include 3rd-party/micro-ecc 3rd-party/yxml

DAEMON_SOURCES = main.c sink.c sink_status.c btstack_audio_ios.c btstack_control_ipod.c hci_transport_h4_ipod.c media_controls.m
