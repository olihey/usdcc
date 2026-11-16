# Explicit upstream translation units. No upstream source is relicensed or copied.
set(BLENDER_MESH_SOURCES
    intern/guardedalloc/intern/leak_detector.cc
    intern/guardedalloc/intern/mallocn.cc
    intern/guardedalloc/intern/mallocn_guarded_impl.cc
    intern/guardedalloc/intern/mallocn_lockfree_impl.cc
    intern/guardedalloc/intern/memory_usage.cc
    intern/clog/clog.cc
    intern/utfconv/utfconv.cc
    intern/eigen/intern/eigenvalues.cc
    intern/eigen/intern/svd.cc
    intern/eigen/intern/matrix.cc
    intern/eigen/intern/linear_solver.cc
    extern/wcwidth/wcwidth.c
    source/blender/blentranslation/intern/blt_translation.cc
)
set(_bmesh_intern callback_generic core delete edgeloop interp iterators marking
    mesh mesh_debug mesh_duplicate mesh_normals mesh_partial_update mesh_tessellate mesh_validate
    mods polygon polygon_edgenet query query_uv structure uvselect walkers walkers_impl)
foreach(_name IN LISTS _bmesh_intern)
    list(APPEND BLENDER_MESH_SOURCES "source/blender/bmesh/intern/bmesh_${_name}.cc")
endforeach()
foreach(_name IN ITEMS beautify bevel edgenet triangulate decimate_collapse decimate_dissolve)
    list(APPEND BLENDER_MESH_SOURCES "source/blender/bmesh/tools/bmesh_${_name}.cc")
endforeach()
list(APPEND BLENDER_MESH_SOURCES
    source/blender/bmesh/operators/bmo_removedoubles.cc
    source/blender/bmesh/operators/bmo_normals.cc)
set(_blenlib BLI_assert BLI_dynstr BLI_ghash BLI_ghash_utils BLI_heap BLI_heap_simple BLI_linklist
    BLI_memarena BLI_memblock BLI_mempool array_utils array_utils_c bitmap
    hash_mm2a hash_mm3 hash_tables implicit_sharing index_range index_mask listbase
    math_base math_base_inline math_base_safe_inline math_bits_inline math_color
    math_color_inline math_color_blend_inline math_geom math_geom_inline math_matrix
    math_matrix_c math_rotation math_rotation_c math_solvers math_vector math_vector_inline
    polyfill_2d polyfill_2d_beautify string string_ref string_utf8 string_utils
    task_range task_scheduler task_iterator task_pool threads time virtual_array
    bit_bool_conversion bit_ref bit_span offset_indices resource_scope index_mask_expression
    generic_virtual_array cpp_type cpp_types rct quadric stack)
foreach(_name IN LISTS _blenlib)
    list(APPEND BLENDER_MESH_SOURCES "source/blender/blenlib/intern/${_name}.cc")
endforeach()
