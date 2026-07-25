#if !defined(_WIN32)
#error "The Inochi2D PE fixture is Windows-only"
#endif

#define CS_EXPORT extern "C" __declspec(dllexport) void

CS_EXPORT in_puppet_load() {}
CS_EXPORT in_puppet_free() {}
CS_EXPORT in_puppet_get_parameters() {}
CS_EXPORT in_parameter_get_name() {}
CS_EXPORT in_parameter_get_dimensions() {}
CS_EXPORT in_parameter_set_value() {}
CS_EXPORT in_puppet_update() {}
#if !defined(CS_INOCHI2D_FIXTURE_OMIT_DRAW)
CS_EXPORT in_puppet_draw() {}
#endif
CS_EXPORT in_puppet_get_drawlist() {}
CS_EXPORT in_drawlist_get_commands() {}
CS_EXPORT in_drawlist_get_vertex_data() {}
CS_EXPORT in_drawlist_get_index_data() {}
CS_EXPORT in_texture_get_width() {}
CS_EXPORT in_texture_get_height() {}
CS_EXPORT in_texture_get_channels() {}
CS_EXPORT in_texture_get_pixels() {}
