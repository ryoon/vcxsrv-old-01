/*
 * Copyright © 2010 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * \file linker.cpp
 * GLSL linker implementation
 *
 * Given a set of shaders that are to be linked to generate a final program,
 * there are three distinct stages.
 *
 * In the first stage shaders are partitioned into groups based on the shader
 * type.  All shaders of a particular type (e.g., vertex shaders) are linked
 * together.
 *
 *   - Undefined references in each shader are resolve to definitions in
 *     another shader.
 *   - Types and qualifiers of uniforms, outputs, and global variables defined
 *     in multiple shaders with the same name are verified to be the same.
 *   - Initializers for uniforms and global variables defined
 *     in multiple shaders with the same name are verified to be the same.
 *
 * The result, in the terminology of the GLSL spec, is a set of shader
 * executables for each processing unit.
 *
 * After the first stage is complete, a series of semantic checks are performed
 * on each of the shader executables.
 *
 *   - Each shader executable must define a \c main function.
 *   - Each vertex shader executable must write to \c gl_Position.
 *   - Each fragment shader executable must write to either \c gl_FragData or
 *     \c gl_FragColor.
 *
 * In the final stage individual shader executables are linked to create a
 * complete exectuable.
 *
 *   - Types of uniforms defined in multiple shader stages with the same name
 *     are verified to be the same.
 *   - Initializers for uniforms defined in multiple shader stages with the
 *     same name are verified to be the same.
 *   - Types and qualifiers of outputs defined in one stage are verified to
 *     be the same as the types and qualifiers of inputs defined with the same
 *     name in a later stage.
 *
 * \author Ian Romanick <ian.d.romanick@intel.com>
 */

#include <ctype.h>
#include "util/strndup.h"
#include "main/core.h"
#include "glsl_symbol_table.h"
#include "glsl_parser_extras.h"
#include "ir.h"
#include "program.h"
#include "program/hash_table.h"
#include "program/prog_instruction.h"
#include "linker.h"
#include "link_varyings.h"
#include "ir_optimization.h"
#include "ir_rvalue_visitor.h"
#include "ir_uniform.h"

#include "main/shaderobj.h"
#include "main/enums.h"


namespace {

/**
 * Visitor that determines whether or not a variable is ever written.
 */
class find_assignment_visitor : public ir_hierarchical_visitor {
public:
   find_assignment_visitor(const char *name)
      : name(name), found(false)
   {
      /* empty */
   }

   virtual ir_visitor_status visit_enter(ir_assignment *ir)
   {
      ir_variable *const var = ir->lhs->variable_referenced();

      if (strcmp(name, var->name) == 0) {
	 found = true;
	 return visit_stop;
      }

      return visit_continue_with_parent;
   }

   virtual ir_visitor_status visit_enter(ir_call *ir)
   {
      foreach_two_lists(formal_node, &ir->callee->parameters,
                        actual_node, &ir->actual_parameters) {
	 ir_rvalue *param_rval = (ir_rvalue *) actual_node;
	 ir_variable *sig_param = (ir_variable *) formal_node;

	 if (sig_param->data.mode == ir_var_function_out ||
	     sig_param->data.mode == ir_var_function_inout) {
	    ir_variable *var = param_rval->variable_referenced();
	    if (var && strcmp(name, var->name) == 0) {
	       found = true;
	       return visit_stop;
	    }
	 }
      }

      if (ir->return_deref != NULL) {
	 ir_variable *const var = ir->return_deref->variable_referenced();

	 if (strcmp(name, var->name) == 0) {
	    found = true;
	    return visit_stop;
	 }
      }

      return visit_continue_with_parent;
   }

   bool variable_found()
   {
      return found;
   }

private:
   const char *name;       /**< Find writes to a variable with this name. */
   bool found;             /**< Was a write to the variable found? */
};


/**
 * Visitor that determines whether or not a variable is ever read.
 */
class find_deref_visitor : public ir_hierarchical_visitor {
public:
   find_deref_visitor(const char *name)
      : name(name), found(false)
   {
      /* empty */
   }

   virtual ir_visitor_status visit(ir_dereference_variable *ir)
   {
      if (strcmp(this->name, ir->var->name) == 0) {
	 this->found = true;
	 return visit_stop;
      }

      return visit_continue;
   }

   bool variable_found() const
   {
      return this->found;
   }

private:
   const char *name;       /**< Find writes to a variable with this name. */
   bool found;             /**< Was a write to the variable found? */
};


class geom_array_resize_visitor : public ir_hierarchical_visitor {
public:
   unsigned num_vertices;
   gl_shader_program *prog;

   geom_array_resize_visitor(unsigned num_vertices, gl_shader_program *prog)
   {
      this->num_vertices = num_vertices;
      this->prog = prog;
   }

   virtual ~geom_array_resize_visitor()
   {
      /* empty */
   }

   virtual ir_visitor_status visit(ir_variable *var)
   {
      if (!var->type->is_array() || var->data.mode != ir_var_shader_in)
         return visit_continue;

      unsigned size = var->type->length;

      /* Generate a link error if the shader has declared this array with an
       * incorrect size.
       */
      if (!var->data.implicit_sized_array &&
          size && size != this->num_vertices) {
         linker_error(this->prog, "size of array %s declared as %u, "
                      "but number of input vertices is %u\n",
                      var->name, size, this->num_vertices);
         return visit_continue;
      }

      /* Generate a link error if the shader attempts to access an input
       * array using an index too large for its actual size assigned at link
       * time.
       */
      if (var->data.max_array_access >= (int)this->num_vertices) {
         linker_error(this->prog, "geometry shader accesses element %i of "
                      "%s, but only %i input vertices\n",
                      var->data.max_array_access, var->name, this->num_vertices);
         return visit_continue;
      }

      var->type = glsl_type::get_array_instance(var->type->fields.array,
                                                this->num_vertices);
      var->data.max_array_access = this->num_vertices - 1;

      return visit_continue;
   }

   /* Dereferences of input variables need to be updated so that their type
    * matches the newly assigned type of the variable they are accessing. */
   virtual ir_visitor_status visit(ir_dereference_variable *ir)
   {
      ir->type = ir->var->type;
      return visit_continue;
   }

   /* Dereferences of 2D input arrays need to be updated so that their type
    * matches the newly assigned type of the array they are accessing. */
   virtual ir_visitor_status visit_leave(ir_dereference_array *ir)
   {
      const glsl_type *const vt = ir->array->type;
      if (vt->is_array())
         ir->type = vt->fields.array;
      return visit_continue;
   }
};

class tess_eval_array_resize_visitor : public ir_hierarchical_visitor {
public:
   unsigned num_vertices;
   gl_shader_program *prog;

   tess_eval_array_resize_visitor(unsigned num_vertices, gl_shader_program *prog)
   {
      this->num_vertices = num_vertices;
      this->prog = prog;
   }

   virtual ~tess_eval_array_resize_visitor()
   {
      /* empty */
   }

   virtual ir_visitor_status visit(ir_variable *var)
   {
      if (!var->type->is_array() || var->data.mode != ir_var_shader_in || var->data.patch)
         return visit_continue;

      var->type = glsl_type::get_array_instance(var->type->fields.array,
                                                this->num_vertices);
      var->data.max_array_access = this->num_vertices - 1;

      return visit_continue;
   }

   /* Dereferences of input variables need to be updated so that their type
    * matches the newly assigned type of the variable they are accessing. */
   virtual ir_visitor_status visit(ir_dereference_variable *ir)
   {
      ir->type = ir->var->type;
      return visit_continue;
   }

   /* Dereferences of 2D input arrays need to be updated so that their type
    * matches the newly assigned type of the array they are accessing. */
   virtual ir_visitor_status visit_leave(ir_dereference_array *ir)
   {
      const glsl_type *const vt = ir->array->type;
      if (vt->is_array())
         ir->type = vt->fields.array;
      return visit_continue;
   }
};

class barrier_use_visitor : public ir_hierarchical_visitor {
public:
   barrier_use_visitor(gl_shader_program *prog)
      : prog(prog), in_main(false), after_return(false), control_flow(0)
   {
   }

   virtual ~barrier_use_visitor()
   {
      /* empty */
   }

   virtual ir_visitor_status visit_enter(ir_function *ir)
   {
      if (strcmp(ir->name, "main") == 0)
         in_main = true;

      return visit_continue;
   }

   virtual ir_visitor_status visit_leave(ir_function *)
   {
      in_main = false;
      after_return = false;
      return visit_continue;
   }

   virtual ir_visitor_status visit_leave(ir_return *)
   {
      after_return = true;
      return visit_continue;
   }

   virtual ir_visitor_status visit_enter(ir_if *)
   {
      ++control_flow;
      return visit_continue;
   }

   virtual ir_visitor_status visit_leave(ir_if *)
   {
      --control_flow;
      return visit_continue;
   }

   virtual ir_visitor_status visit_enter(ir_loop *)
   {
      ++control_flow;
      return visit_continue;
   }

   virtual ir_visitor_status visit_leave(ir_loop *)
   {
      --control_flow;
      return visit_continue;
   }

   /* FINISHME: `switch` is not expressed at the IR level -- it's already
    * been lowered to a mess of `if`s. We'll correctly disallow any use of
    * barrier() in a conditional path within the switch, but not in a path
    * which is always hit.
    */

   virtual ir_visitor_status visit_enter(ir_call *ir)
   {
      if (ir->use_builtin && strcmp(ir->callee_name(), "barrier") == 0) {
         /* Use of barrier(); determine if it is legal: */
         if (!in_main) {
            linker_error(prog, "Builtin barrier() may only be used in main");
            return visit_stop;
         }

         if (after_return) {
            linker_error(prog, "Builtin barrier() may not be used after return");
            return visit_stop;
         }

         if (control_flow != 0) {
            linker_error(prog, "Builtin barrier() may not be used inside control flow");
            return visit_stop;
         }
      }
      return visit_continue;
   }

private:
   gl_shader_program *prog;
   bool in_main, after_return;
   int control_flow;
};

/**
 * Visitor that determines the highest stream id to which a (geometry) shader
 * emits vertices. It also checks whether End{Stream}Primitive is ever called.
 */
class find_emit_vertex_visitor : public ir_hierarchical_visitor {
public:
   find_emit_vertex_visitor(int max_allowed)
      : max_stream_allowed(max_allowed),
        invalid_stream_id(0),
        invalid_stream_id_from_emit_vertex(false),
        end_primitive_found(false),
        uses_non_zero_stream(false)
   {
      /* empty */
   }

   virtual ir_visitor_status visit_leave(ir_emit_vertex *ir)
   {
      int stream_id = ir->stream_id();

      if (stream_id < 0) {
         invalid_stream_id = stream_id;
         invalid_stream_id_from_emit_vertex = true;
         return visit_stop;
      }

      if (stream_id > max_stream_allowed) {
         invalid_stream_id = stream_id;
         invalid_stream_id_from_emit_vertex = true;
         return visit_stop;
      }

      if (stream_id != 0)
         uses_non_zero_stream = true;

      return visit_continue;
   }

   virtual ir_visitor_status visit_leave(ir_end_primitive *ir)
   {
      end_primitive_found = true;

      int stream_id = ir->stream_id();

      if (stream_id < 0) {
         invalid_stream_id = stream_id;
         invalid_stream_id_from_emit_vertex = false;
         return visit_stop;
      }

      if (stream_id > max_stream_allowed) {
         invalid_stream_id = stream_id;
         invalid_stream_id_from_emit_vertex = false;
         return visit_stop;
      }

      if (stream_id != 0)
         uses_non_zero_stream = true;

      return visit_continue;
   }

   bool error()
   {
      return invalid_stream_id != 0;
   }

   const char *error_func()
   {
      return invalid_stream_id_from_emit_vertex ?
         "EmitStreamVertex" : "EndStreamPrimitive";
   }

   int error_stream()
   {
      return invalid_stream_id;
   }

   bool uses_streams()
   {
      return uses_non_zero_stream;
   }

   bool uses_end_primitive()
   {
      return end_primitive_found;
   }

private:
   int max_stream_allowed;
   int invalid_stream_id;
   bool invalid_stream_id_from_emit_vertex;
   bool end_primitive_found;
   bool uses_non_zero_stream;
};

/* Class that finds array derefs and check if indexes are dynamic. */
class dynamic_sampler_array_indexing_visitor : public ir_hierarchical_visitor
{
public:
   dynamic_sampler_array_indexing_visitor() :
      dynamic_sampler_array_indexing(false)
   {
   }

   ir_visitor_status visit_enter(ir_dereference_array *ir)
   {
      if (!ir->variable_referenced())
         return visit_continue;

      if (!ir->variable_referenced()->type->contains_sampler())
         return visit_continue;

      if (!ir->array_index->constant_expression_value()) {
         dynamic_sampler_array_indexing = true;
         return visit_stop;
      }
      return visit_continue;
   }

   bool uses_dynamic_sampler_array_indexing()
   {
      return dynamic_sampler_array_indexing;
   }

private:
   bool dynamic_sampler_array_indexing;
};

} /* anonymous namespace */

void
linker_error(gl_shader_program *prog, const char *fmt, ...)
{
   va_list ap;

   ralloc_strcat(&prog->InfoLog, "error: ");
   va_start(ap, fmt);
   ralloc_vasprintf_append(&prog->InfoLog, fmt, ap);
   va_end(ap);

   prog->LinkStatus = false;
}


void
linker_warning(gl_shader_program *prog, const char *fmt, ...)
{
   va_list ap;

   ralloc_strcat(&prog->InfoLog, "warning: ");
   va_start(ap, fmt);
   ralloc_vasprintf_append(&prog->InfoLog, fmt, ap);
   va_end(ap);

}


/**
 * Given a string identifying a program resource, break it into a base name
 * and an optional array index in square brackets.
 *
 * If an array index is present, \c out_base_name_end is set to point to the
 * "[" that precedes the array index, and the array index itself is returned
 * as a long.
 *
 * If no array index is present (or if the array index is negative or
 * mal-formed), \c out_base_name_end, is set to point to the null terminator
 * at the end of the input string, and -1 is returned.
 *
 * Only the final array index is parsed; if the string contains other array
 * indices (or structure field accesses), they are left in the base name.
 *
 * No attempt is made to check that the base name is properly formed;
 * typically the caller will look up the base name in a hash table, so
 * ill-formed base names simply turn into hash table lookup failures.
 */
long
parse_program_resource_name(const GLchar *name,
                            const GLchar **out_base_name_end)
{
   /* Section 7.3.1 ("Program Interfaces") of the OpenGL 4.3 spec says:
    *
    *     "When an integer array element or block instance number is part of
    *     the name string, it will be specified in decimal form without a "+"
    *     or "-" sign or any extra leading zeroes. Additionally, the name
    *     string will not include white space anywhere in the string."
    */

   const size_t len = strlen(name);
   *out_base_name_end = name + len;

   if (len == 0 || name[len-1] != ']')
      return -1;

   /* Walk backwards over the string looking for a non-digit character.  This
    * had better be the opening bracket for an array index.
    *
    * Initially, i specifies the location of the ']'.  Since the string may
    * contain only the ']' charcater, walk backwards very carefully.
    */
   unsigned i;
   for (i = len - 1; (i > 0) && isdigit(name[i-1]); --i)
      /* empty */ ;

   if ((i == 0) || name[i-1] != '[')
      return -1;

   long array_index = strtol(&name[i], NULL, 10);
   if (array_index < 0)
      return -1;

   /* Check for leading zero */
   if (name[i] == '0' && name[i+1] != ']')
      return -1;

   *out_base_name_end = name + (i - 1);
   return array_index;
}


void
link_invalidate_variable_locations(exec_list *ir)
{
   foreach_in_list(ir_instruction, node, ir) {
      ir_variable *const var = node->as_variable();

      if (var == NULL)
         continue;

      /* Only assign locations for variables that lack an explicit location.
       * Explicit locations are set for all built-in variables, generic vertex
       * shader inputs (via layout(location=...)), and generic fragment shader
       * outputs (also via layout(location=...)).
       */
      if (!var->data.explicit_location) {
         var->data.location = -1;
         var->data.location_frac = 0;
      }

      /* ir_variable::is_unmatched_generic_inout is used by the linker while
       * connecting outputs from one stage to inputs of the next stage.
       */
      if (var->data.explicit_location &&
          var->data.location < VARYING_SLOT_VAR0) {
         var->data.is_unmatched_generic_inout = 0;
      } else {
         var->data.is_unmatched_generic_inout = 1;
      }
   }
}


/**
 * Set clip_distance_array_size based and cull_distance_array_size on the given
 * shader.
 *
 * Also check for errors based on incorrect usage of gl_ClipVertex and
 * gl_ClipDistance and gl_CullDistance.
 * Additionally test whether the arrays gl_ClipDistance and gl_CullDistance
 * exceed the maximum size defined by gl_MaxCombinedClipAndCullDistances.
 *
 * Return false if an error was reported.
 */
static void
analyze_clip_cull_usage(struct gl_shader_program *prog,
                        struct gl_shader *shader,
                        struct gl_context *ctx,
                        GLuint *clip_distance_array_size,
                        GLuint *cull_distance_array_size)
{
   *clip_distance_array_size = 0;
   *cull_distance_array_size = 0;

   if (prog->Version >= (prog->IsES ? 300 : 130)) {
      /* From section 7.1 (Vertex Shader Special Variables) of the
       * GLSL 1.30 spec:
       *
       *   "It is an error for a shader to statically write both
       *   gl_ClipVertex and gl_ClipDistance."
       *
       * This does not apply to GLSL ES shaders, since GLSL ES defines neither
       * gl_ClipVertex nor gl_ClipDistance. However with
       * GL_EXT_clip_cull_distance, this functionality is exposed in ES 3.0.
       */
      find_assignment_visitor clip_distance("gl_ClipDistance");
      find_assignment_visitor cull_distance("gl_CullDistance");

      clip_distance.run(shader->ir);
      cull_distance.run(shader->ir);

      /* From the ARB_cull_distance spec:
       *
       * It is a compile-time or link-time error for the set of shaders forming
       * a program to statically read or write both gl_ClipVertex and either
       * gl_ClipDistance or gl_CullDistance.
       *
       * This does not apply to GLSL ES shaders, since GLSL ES doesn't define
       * gl_ClipVertex.
       */
      if (!prog->IsES) {
         find_assignment_visitor clip_vertex("gl_ClipVertex");

         clip_vertex.run(shader->ir);

         if (clip_vertex.variable_found() && clip_distance.variable_found()) {
            linker_error(prog, "%s shader writes to both `gl_ClipVertex' "
                         "and `gl_ClipDistance'\n",
                         _mesa_shader_stage_to_string(shader->Stage));
            return;
         }
         if (clip_vertex.variable_found() && cull_distance.variable_found()) {
            linker_error(prog, "%s shader writes to both `gl_ClipVertex' "
                         "and `gl_CullDistance'\n",
                         _mesa_shader_stage_to_string(shader->Stage));
            return;
         }
      }

      if (clip_distance.variable_found()) {
         ir_variable *clip_distance_var =
                shader->symbols->get_variable("gl_ClipDistance");
         assert(clip_distance_var);
         *clip_distance_array_size = clip_distance_var->type->length;
      }
      if (cull_distance.variable_found()) {
         ir_variable *cull_distance_var =
                shader->symbols->get_variable("gl_CullDistance");
         assert(cull_distance_var);
         *cull_distance_array_size = cull_distance_var->type->length;
      }
      /* From the ARB_cull_distance spec:
       *
       * It is a compile-time or link-time error for the set of shaders forming
       * a program to have the sum of the sizes of the gl_ClipDistance and
       * gl_CullDistance arrays to be larger than
       * gl_MaxCombinedClipAndCullDistances.
       */
      if ((*clip_distance_array_size + *cull_distance_array_size) >
          ctx->Const.MaxClipPlanes) {
          linker_error(prog, "%s shader: the combined size of "
                       "'gl_ClipDistance' and 'gl_CullDistance' size cannot "
                       "be larger than "
                       "gl_MaxCombinedClipAndCullDistances (%u)",
                       _mesa_shader_stage_to_string(shader->Stage),
                       ctx->Const.MaxClipPlanes);
      }
   }
}


/**
 * Verify that a vertex shader executable meets all semantic requirements.
 *
 * Also sets prog->Vert.ClipDistanceArraySize and
 * prog->Vert.CullDistanceArraySize as a side effect.
 *
 * \param shader  Vertex shader executable to be verified
 */
void
validate_vertex_shader_executable(struct gl_shader_program *prog,
                                  struct gl_shader *shader,
                                  struct gl_context *ctx)
{
   if (shader == NULL)
      return;

   /* From the GLSL 1.10 spec, page 48:
    *
    *     "The variable gl_Position is available only in the vertex
    *      language and is intended for writing the homogeneous vertex
    *      position. All executions of a well-formed vertex shader
    *      executable must write a value into this variable. [...] The
    *      variable gl_Position is available only in the vertex
    *      language and is intended for writing the homogeneous vertex
    *      position. All executions of a well-formed vertex shader
    *      executable must write a value into this variable."
    *
    * while in GLSL 1.40 this text is changed to:
    *
    *     "The variable gl_Position is available only in the vertex
    *      language and is intended for writing the homogeneous vertex
    *      position. It can be written at any time during shader
    *      execution. It may also be read back by a vertex shader
    *      after being written. This value will be used by primitive
    *      assembly, clipping, culling, and other fixed functionality
    *      operations, if present, that operate on primitives after
    *      vertex processing has occurred. Its value is undefined if
    *      the vertex shader executable does not write gl_Position."
    *
    * All GLSL ES Versions are similar to GLSL 1.40--failing to write to
    * gl_Position is not an error.
    */
   if (prog->Version < (prog->IsES ? 300 : 140)) {
      find_assignment_visitor find("gl_Position");
      find.run(shader->ir);
      if (!find.variable_found()) {
        if (prog->IsES) {
          linker_warning(prog,
                         "vertex shader does not write to `gl_Position'."
                         "It's value is undefined. \n");
        } else {
          linker_error(prog,
                       "vertex shader does not write to `gl_Position'. \n");
        }
	 return;
      }
   }

   analyze_clip_cull_usage(prog, shader, ctx,
                           &prog->Vert.ClipDistanceArraySize,
                           &prog->Vert.CullDistanceArraySize);
}

void
validate_tess_eval_shader_executable(struct gl_shader_program *prog,
                                     struct gl_shader *shader,
                                     struct gl_context *ctx)
{
   if (shader == NULL)
      return;

   analyze_clip_cull_usage(prog, shader, ctx,
                           &prog->TessEval.ClipDistanceArraySize,
                           &prog->TessEval.CullDistanceArraySize);
}


/**
 * Verify that a fragment shader executable meets all semantic requirements
 *
 * \param shader  Fragment shader executable to be verified
 */
void
validate_fragment_shader_executable(struct gl_shader_program *prog,
                                    struct gl_shader *shader)
{
   if (shader == NULL)
      return;

   find_assignment_visitor frag_color("gl_FragColor");
   find_assignment_visitor frag_data("gl_FragData");

   frag_color.run(shader->ir);
   frag_data.run(shader->ir);

   if (frag_color.variable_found() && frag_data.variable_found()) {
      linker_error(prog,  "fragment shader writes to both "
		   "`gl_FragColor' and `gl_FragData'\n");
   }
}

/**
 * Verify that a geometry shader executable meets all semantic requirements
 *
 * Also sets prog->Geom.VerticesIn, and prog->Geom.ClipDistanceArraySize and
 * prog->Geom.CullDistanceArraySize as a side effect.
 *
 * \param shader Geometry shader executable to be verified
 */
void
validate_geometry_shader_executable(struct gl_shader_program *prog,
                                    struct gl_shader *shader,
                                    struct gl_context *ctx)
{
   if (shader == NULL)
      return;

   unsigned num_vertices = vertices_per_prim(prog->Geom.InputType);
   prog->Geom.VerticesIn = num_vertices;

   analyze_clip_cull_usage(prog, shader, ctx,
                           &prog->Geom.ClipDistanceArraySize,
                           &prog->Geom.CullDistanceArraySize);
}

/**
 * Check if geometry shaders emit to non-zero streams and do corresponding
 * validations.
 */
static void
validate_geometry_shader_emissions(struct gl_context *ctx,
                                   struct gl_shader_program *prog)
{
   if (prog->_LinkedShaders[MESA_SHADER_GEOMETRY] != NULL) {
      find_emit_vertex_visitor emit_vertex(ctx->Const.MaxVertexStreams - 1);
      emit_vertex.run(prog->_LinkedShaders[MESA_SHADER_GEOMETRY]->ir);
      if (emit_vertex.error()) {
         linker_error(prog, "Invalid call %s(%d). Accepted values for the "
                      "stream parameter are in the range [0, %d].\n",
                      emit_vertex.error_func(),
                      emit_vertex.error_stream(),
                      ctx->Const.MaxVertexStreams - 1);
      }
      prog->Geom.UsesStreams = emit_vertex.uses_streams();
      prog->Geom.UsesEndPrimitive = emit_vertex.uses_end_primitive();

      /* From the ARB_gpu_shader5 spec:
       *
       *   "Multiple vertex streams are supported only if the output primitive
       *    type is declared to be "points".  A program will fail to link if it
       *    contains a geometry shader calling EmitStreamVertex() or
       *    EndStreamPrimitive() if its output primitive type is not "points".
       *
       * However, in the same spec:
       *
       *   "The function EmitVertex() is equivalent to calling EmitStreamVertex()
       *    with <stream> set to zero."
       *
       * And:
       *
       *   "The function EndPrimitive() is equivalent to calling
       *    EndStreamPrimitive() with <stream> set to zero."
       *
       * Since we can call EmitVertex() and EndPrimitive() when we output
       * primitives other than points, calling EmitStreamVertex(0) or
       * EmitEndPrimitive(0) should not produce errors. This it also what Nvidia
       * does. Currently we only set prog->Geom.UsesStreams to TRUE when
       * EmitStreamVertex() or EmitEndPrimitive() are called with a non-zero
       * stream.
       */
      if (prog->Geom.UsesStreams && prog->Geom.OutputType != GL_POINTS) {
         linker_error(prog, "EmitStreamVertex(n) and EndStreamPrimitive(n) "
                      "with n>0 requires point output\n");
      }
   }
}

bool
validate_intrastage_arrays(struct gl_shader_program *prog,
                           ir_variable *const var,
		           ir_variable *const existing)
{
   /* Consider the types to be "the same" if both types are arrays
    * of the same type and one of the arrays is implicitly sized.
    * In addition, set the type of the linked variable to the
    * explicitly sized array.
    */
   if (var->type->is_array() && existing->type->is_array()) {
      if ((var->type->fields.array == existing->type->fields.array) &&
          ((var->type->length == 0)|| (existing->type->length == 0))) {
         if (var->type->length != 0) {
            if ((int)var->type->length <= existing->data.max_array_access) {
               linker_error(prog, "%s `%s' declared as type "
                           "`%s' but outermost dimension has an index"
                           " of `%i'\n",
                           mode_string(var),
                           var->name, var->type->name,
                           existing->data.max_array_access);
            }
            existing->type = var->type;
            return true;
         } else if (existing->type->length != 0) {
            if((int)existing->type->length <= var->data.max_array_access &&
               !existing->data.from_ssbo_unsized_array) {
               linker_error(prog, "%s `%s' declared as type "
                           "`%s' but outermost dimension has an index"
                           " of `%i'\n",
                           mode_string(var),
                           var->name, existing->type->name,
                           var->data.max_array_access);
            }
            return true;
         }
      } else {
         /* The arrays of structs could have different glsl_type pointers but
          * they are actually the same type. Use record_compare() to check that.
          */
         if (existing->type->fields.array->is_record() &&
             var->type->fields.array->is_record() &&
             existing->type->fields.array->record_compare(var->type->fields.array))
            return true;
      }
   }
   return false;
}


/**
 * Perform validation of global variables used across multiple shaders
 */
void
cross_validate_globals(struct gl_shader_program *prog,
		       struct gl_shader **shader_list,
		       unsigned num_shaders,
		       bool uniforms_only)
{
   /* Examine all of the uniforms in all of the shaders and cross validate
    * them.
    */
   glsl_symbol_table variables;
   for (unsigned i = 0; i < num_shaders; i++) {
      if (shader_list[i] == NULL)
	 continue;

      foreach_in_list(ir_instruction, node, shader_list[i]->ir) {
	 ir_variable *const var = node->as_variable();

	 if (var == NULL)
	    continue;

	 if (uniforms_only && (var->data.mode != ir_var_uniform && var->data.mode != ir_var_shader_storage))
	    continue;

         /* don't cross validate subroutine uniforms */
         if (var->type->contains_subroutine())
            continue;

	 /* Don't cross validate temporaries that are at global scope.  These
	  * will eventually get pulled into the shaders 'main'.
	  */
	 if (var->data.mode == ir_var_temporary)
	    continue;

	 /* If a global with this name has already been seen, verify that the
	  * new instance has the same type.  In addition, if the globals have
	  * initializers, the values of the initializers must be the same.
	  */
	 ir_variable *const existing = variables.get_variable(var->name);
	 if (existing != NULL) {
            /* Check if types match. Interface blocks have some special
             * rules so we handle those elsewhere.
             */
           if (var->type != existing->type &&
                !var->is_interface_instance()) {
	       if (!validate_intrastage_arrays(prog, var, existing)) {
                  if (var->type->is_record() && existing->type->is_record()
                      && existing->type->record_compare(var->type)) {
                     existing->type = var->type;
                  } else {
                     /* If it is an unsized array in a Shader Storage Block,
                      * two different shaders can access to different elements.
                      * Because of that, they might be converted to different
                      * sized arrays, then check that they are compatible but
                      * ignore the array size.
                      */
                     if (!(var->data.mode == ir_var_shader_storage &&
                           var->data.from_ssbo_unsized_array &&
                           existing->data.mode == ir_var_shader_storage &&
                           existing->data.from_ssbo_unsized_array &&
                           var->type->gl_type == existing->type->gl_type)) {
                        linker_error(prog, "%s `%s' declared as type "
                                    "`%s' and type `%s'\n",
                                    mode_string(var),
                                    var->name, var->type->name,
                                    existing->type->name);
                        return;
                     }
                  }
	       }
	    }

	    if (var->data.explicit_location) {
	       if (existing->data.explicit_location
		   && (var->data.location != existing->data.location)) {
		     linker_error(prog, "explicit locations for %s "
				  "`%s' have differing values\n",
				  mode_string(var), var->name);
		     return;
	       }

	       if (var->data.location_frac != existing->data.location_frac) {
		     linker_error(prog, "explicit components for %s "
				  "`%s' have differing values\n",
				  mode_string(var), var->name);
		     return;
	       }

	       existing->data.location = var->data.location;
	       existing->data.explicit_location = true;
	    } else {
               /* Check if uniform with implicit location was marked explicit
                * by earlier shader stage. If so, mark it explicit in this stage
                * too to make sure later processing does not treat it as
                * implicit one.
                */
               if (existing->data.explicit_location) {
	          var->data.location = existing->data.location;
	          var->data.explicit_location = true;
               }
            }

            /* From the GLSL 4.20 specification:
             * "A link error will result if two compilation units in a program
             *  specify different integer-constant bindings for the same
             *  opaque-uniform name.  However, it is not an error to specify a
             *  binding on some but not all declarations for the same name"
             */
            if (var->data.explicit_binding) {
               if (existing->data.explicit_binding &&
                   var->data.binding != existing->data.binding) {
                  linker_error(prog, "explicit bindings for %s "
                               "`%s' have differing values\n",
                               mode_string(var), var->name);
                  return;
               }

               existing->data.binding = var->data.binding;
               existing->data.explicit_binding = true;
            }

            if (var->type->contains_atomic() &&
                var->data.offset != existing->data.offset) {
               linker_error(prog, "offset specifications for %s "
                            "`%s' have differing values\n",
                            mode_string(var), var->name);
               return;
            }

	    /* Validate layout qualifiers for gl_FragDepth.
	     *
	     * From the AMD/ARB_conservative_depth specs:
	     *
	     *    "If gl_FragDepth is redeclared in any fragment shader in a
	     *    program, it must be redeclared in all fragment shaders in
	     *    that program that have static assignments to
	     *    gl_FragDepth. All redeclarations of gl_FragDepth in all
	     *    fragment shaders in a single program must have the same set
	     *    of qualifiers."
	     */
	    if (strcmp(var->name, "gl_FragDepth") == 0) {
	       bool layout_declared = var->data.depth_layout != ir_depth_layout_none;
	       bool layout_differs =
		  var->data.depth_layout != existing->data.depth_layout;

	       if (layout_declared && layout_differs) {
		  linker_error(prog,
			       "All redeclarations of gl_FragDepth in all "
			       "fragment shaders in a single program must have "
			       "the same set of qualifiers.\n");
	       }

	       if (var->data.used && layout_differs) {
		  linker_error(prog,
			       "If gl_FragDepth is redeclared with a layout "
			       "qualifier in any fragment shader, it must be "
			       "redeclared with the same layout qualifier in "
			       "all fragment shaders that have assignments to "
			       "gl_FragDepth\n");
	       }
	    }

	    /* Page 35 (page 41 of the PDF) of the GLSL 4.20 spec says:
	     *
	     *     "If a shared global has multiple initializers, the
	     *     initializers must all be constant expressions, and they
	     *     must all have the same value. Otherwise, a link error will
	     *     result. (A shared global having only one initializer does
	     *     not require that initializer to be a constant expression.)"
	     *
	     * Previous to 4.20 the GLSL spec simply said that initializers
	     * must have the same value.  In this case of non-constant
	     * initializers, this was impossible to determine.  As a result,
	     * no vendor actually implemented that behavior.  The 4.20
	     * behavior matches the implemented behavior of at least one other
	     * vendor, so we'll implement that for all GLSL versions.
	     */
	    if (var->constant_initializer != NULL) {
	       if (existing->constant_initializer != NULL) {
		  if (!var->constant_initializer->has_value(existing->constant_initializer)) {
		     linker_error(prog, "initializers for %s "
				  "`%s' have differing values\n",
				  mode_string(var), var->name);
		     return;
		  }
	       } else {
                  /* If the first-seen instance of a particular uniform did
                   * not have an initializer but a later instance does,
                   * replace the former with the later.
                   */
                  variables.replace_variable(existing->name, var);
	       }
	    }

	    if (var->data.has_initializer) {
	       if (existing->data.has_initializer
		   && (var->constant_initializer == NULL
		       || existing->constant_initializer == NULL)) {
		  linker_error(prog,
			       "shared global variable `%s' has multiple "
			       "non-constant initializers.\n",
			       var->name);
		  return;
	       }
	    }

	    if (existing->data.invariant != var->data.invariant) {
	       linker_error(prog, "declarations for %s `%s' have "
			    "mismatching invariant qualifiers\n",
			    mode_string(var), var->name);
	       return;
	    }
            if (existing->data.centroid != var->data.centroid) {
               linker_error(prog, "declarations for %s `%s' have "
			    "mismatching centroid qualifiers\n",
			    mode_string(var), var->name);
               return;
            }
            if (existing->data.sample != var->data.sample) {
               linker_error(prog, "declarations for %s `%s` have "
                            "mismatching sample qualifiers\n",
                            mode_string(var), var->name);
               return;
            }
            if (existing->data.image_format != var->data.image_format) {
               linker_error(prog, "declarations for %s `%s` have "
                            "mismatching image format qualifiers\n",
                            mode_string(var), var->name);
               return;
            }
	 } else
	    variables.add_variable(var);
      }
   }
}


/**
 * Perform validation of uniforms used across multiple shader stages
 */
void
cross_validate_uniforms(struct gl_shader_program *prog)
{
   cross_validate_globals(prog, prog->_LinkedShaders,
                          MESA_SHADER_STAGES, true);
}

/**
 * Accumulates the array of buffer blocks and checks that all definitions of
 * blocks agree on their contents.
 */
static bool
interstage_cross_validate_uniform_blocks(struct gl_shader_program *prog,
                                         bool validate_ssbo)
{
   int *InterfaceBlockStageIndex[MESA_SHADER_STAGES];
   struct gl_uniform_block *blks = NULL;
   unsigned *num_blks = validate_ssbo ? &prog->NumShaderStorageBlocks :
      &prog->NumUniformBlocks;

   unsigned max_num_buffer_blocks = 0;
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      if (prog->_LinkedShaders[i]) {
         if (validate_ssbo) {
            max_num_buffer_blocks +=
               prog->_LinkedShaders[i]->NumShaderStorageBlocks;
         } else {
            max_num_buffer_blocks +=
               prog->_LinkedShaders[i]->NumUniformBlocks;
         }
      }
   }

   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      struct gl_shader *sh = prog->_LinkedShaders[i];

      InterfaceBlockStageIndex[i] = new int[max_num_buffer_blocks];
      for (unsigned int j = 0; j < max_num_buffer_blocks; j++)
         InterfaceBlockStageIndex[i][j] = -1;

      if (sh == NULL)
	 continue;

      unsigned sh_num_blocks;
      struct gl_uniform_block **sh_blks;
      if (validate_ssbo) {
         sh_num_blocks = prog->_LinkedShaders[i]->NumShaderStorageBlocks;
         sh_blks = sh->ShaderStorageBlocks;
      } else {
         sh_num_blocks = prog->_LinkedShaders[i]->NumUniformBlocks;
         sh_blks = sh->UniformBlocks;
      }

      for (unsigned int j = 0; j < sh_num_blocks; j++) {
         int index = link_cross_validate_uniform_block(prog, &blks, num_blks,
                                                       sh_blks[j]);

         if (index == -1) {
            linker_error(prog, "buffer block `%s' has mismatching "
                         "definitions\n", sh_blks[j]->Name);

            for (unsigned k = 0; k <= i; k++) {
               delete[] InterfaceBlockStageIndex[k];
            }
            return false;
         }

         InterfaceBlockStageIndex[i][index] = j;
      }
   }

   /* Update per stage block pointers to point to the program list.
    * FIXME: We should be able to free the per stage blocks here.
    */
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      for (unsigned j = 0; j < *num_blks; j++) {
         int stage_index = InterfaceBlockStageIndex[i][j];

	 if (stage_index != -1) {
	    struct gl_shader *sh = prog->_LinkedShaders[i];

            blks[j].stageref |= (1 << i);

            struct gl_uniform_block **sh_blks = validate_ssbo ?
               sh->ShaderStorageBlocks : sh->UniformBlocks;

            sh_blks[stage_index] = &blks[j];
	 }
      }
   }

   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      delete[] InterfaceBlockStageIndex[i];
   }

   if (validate_ssbo)
      prog->ShaderStorageBlocks = blks;
   else
      prog->UniformBlocks = blks;

   return true;
}


/**
 * Populates a shaders symbol table with all global declarations
 */
static void
populate_symbol_table(gl_shader *sh)
{
   sh->symbols = new(sh) glsl_symbol_table;

   foreach_in_list(ir_instruction, inst, sh->ir) {
      ir_variable *var;
      ir_function *func;

      if ((func = inst->as_function()) != NULL) {
	 sh->symbols->add_function(func);
      } else if ((var = inst->as_variable()) != NULL) {
         if (var->data.mode != ir_var_temporary)
            sh->symbols->add_variable(var);
      }
   }
}


/**
 * Remap variables referenced in an instruction tree
 *
 * This is used when instruction trees are cloned from one shader and placed in
 * another.  These trees will contain references to \c ir_variable nodes that
 * do not exist in the target shader.  This function finds these \c ir_variable
 * references and replaces the references with matching variables in the target
 * shader.
 *
 * If there is no matching variable in the target shader, a clone of the
 * \c ir_variable is made and added to the target shader.  The new variable is
 * added to \b both the instruction stream and the symbol table.
 *
 * \param inst         IR tree that is to be processed.
 * \param symbols      Symbol table containing global scope symbols in the
 *                     linked shader.
 * \param instructions Instruction stream where new variable declarations
 *                     should be added.
 */
void
remap_variables(ir_instruction *inst, struct gl_shader *target,
		hash_table *temps)
{
   class remap_visitor : public ir_hierarchical_visitor {
   public:
	 remap_visitor(struct gl_shader *target,
		    hash_table *temps)
      {
	 this->target = target;
	 this->symbols = target->symbols;
	 this->instructions = target->ir;
	 this->temps = temps;
      }

      virtual ir_visitor_status visit(ir_dereference_variable *ir)
      {
	 if (ir->var->data.mode == ir_var_temporary) {
	    ir_variable *var = (ir_variable *) hash_table_find(temps, ir->var);

	    assert(var != NULL);
	    ir->var = var;
	    return visit_continue;
	 }

	 ir_variable *const existing =
	    this->symbols->get_variable(ir->var->name);
	 if (existing != NULL)
	    ir->var = existing;
	 else {
	    ir_variable *copy = ir->var->clone(this->target, NULL);

	    this->symbols->add_variable(copy);
	    this->instructions->push_head(copy);
	    ir->var = copy;
	 }

	 return visit_continue;
      }

   private:
      struct gl_shader *target;
      glsl_symbol_table *symbols;
      exec_list *instructions;
      hash_table *temps;
   };

   remap_visitor v(target, temps);

   inst->accept(&v);
}


/**
 * Move non-declarations from one instruction stream to another
 *
 * The intended usage pattern of this function is to pass the pointer to the
 * head sentinel of a list (i.e., a pointer to the list cast to an \c exec_node
 * pointer) for \c last and \c false for \c make_copies on the first
 * call.  Successive calls pass the return value of the previous call for
 * \c last and \c true for \c make_copies.
 *
 * \param instructions Source instruction stream
 * \param last         Instruction after which new instructions should be
 *                     inserted in the target instruction stream
 * \param make_copies  Flag selecting whether instructions in \c instructions
 *                     should be copied (via \c ir_instruction::clone) into the
 *                     target list or moved.
 *
 * \return
 * The new "last" instruction in the target instruction stream.  This pointer
 * is suitable for use as the \c last parameter of a later call to this
 * function.
 */
exec_node *
move_non_declarations(exec_list *instructions, exec_node *last,
		      bool make_copies, gl_shader *target)
{
   hash_table *temps = NULL;

   if (make_copies)
      temps = hash_table_ctor(0, hash_table_pointer_hash,
			      hash_table_pointer_compare);

   foreach_in_list_safe(ir_instruction, inst, instructions) {
      if (inst->as_function())
	 continue;

      ir_variable *var = inst->as_variable();
      if ((var != NULL) && (var->data.mode != ir_var_temporary))
	 continue;

      assert(inst->as_assignment()
             || inst->as_call()
             || inst->as_if() /* for initializers with the ?: operator */
	     || ((var != NULL) && (var->data.mode == ir_var_temporary)));

      if (make_copies) {
	 inst = inst->clone(target, NULL);

	 if (var != NULL)
	    hash_table_insert(temps, inst, var);
	 else
	    remap_variables(inst, target, temps);
      } else {
	 inst->remove();
      }

      last->insert_after(inst);
      last = inst;
   }

   if (make_copies)
      hash_table_dtor(temps);

   return last;
}


/**
 * This class is only used in link_intrastage_shaders() below but declaring
 * it inside that function leads to compiler warnings with some versions of
 * gcc.
 */
class array_sizing_visitor : public ir_hierarchical_visitor {
public:
   array_sizing_visitor()
      : mem_ctx(ralloc_context(NULL)),
        unnamed_interfaces(hash_table_ctor(0, hash_table_pointer_hash,
                                           hash_table_pointer_compare))
   {
   }

   ~array_sizing_visitor()
   {
      hash_table_dtor(this->unnamed_interfaces);
      ralloc_free(this->mem_ctx);
   }

   virtual ir_visitor_status visit(ir_variable *var)
   {
      const glsl_type *type_without_array;
      bool implicit_sized_array = var->data.implicit_sized_array;
      fixup_type(&var->type, var->data.max_array_access,
                 var->data.from_ssbo_unsized_array,
                 &implicit_sized_array);
      var->data.implicit_sized_array = implicit_sized_array;
      type_without_array = var->type->without_array();
      if (var->type->is_interface()) {
         if (interface_contains_unsized_arrays(var->type)) {
            const glsl_type *new_type =
               resize_interface_members(var->type,
                                        var->get_max_ifc_array_access(),
                                        var->is_in_shader_storage_block());
            var->type = new_type;
            var->change_interface_type(new_type);
         }
      } else if (type_without_array->is_interface()) {
         if (interface_contains_unsized_arrays(type_without_array)) {
            const glsl_type *new_type =
               resize_interface_members(type_without_array,
                                        var->get_max_ifc_array_access(),
                                        var->is_in_shader_storage_block());
            var->change_interface_type(new_type);
            var->type = update_interface_members_array(var->type, new_type);
         }
      } else if (const glsl_type *ifc_type = var->get_interface_type()) {
         /* Store a pointer to the variable in the unnamed_interfaces
          * hashtable.
          */
         ir_variable **interface_vars = (ir_variable **)
            hash_table_find(this->unnamed_interfaces, ifc_type);
         if (interface_vars == NULL) {
            interface_vars = rzalloc_array(mem_ctx, ir_variable *,
                                           ifc_type->length);
            hash_table_insert(this->unnamed_interfaces, interface_vars,
                              ifc_type);
         }
         unsigned index = ifc_type->field_index(var->name);
         assert(index < ifc_type->length);
         assert(interface_vars[index] == NULL);
         interface_vars[index] = var;
      }
      return visit_continue;
   }

   /**
    * For each unnamed interface block that was discovered while running the
    * visitor, adjust the interface type to reflect the newly assigned array
    * sizes, and fix up the ir_variable nodes to point to the new interface
    * type.
    */
   void fixup_unnamed_interface_types()
   {
      hash_table_call_foreach(this->unnamed_interfaces,
                              fixup_unnamed_interface_type, NULL);
   }

private:
   /**
    * If the type pointed to by \c type represents an unsized array, replace
    * it with a sized array whose size is determined by max_array_access.
    */
   static void fixup_type(const glsl_type **type, unsigned max_array_access,
                          bool from_ssbo_unsized_array, bool *implicit_sized)
   {
      if (!from_ssbo_unsized_array && (*type)->is_unsized_array()) {
         *type = glsl_type::get_array_instance((*type)->fields.array,
                                               max_array_access + 1);
         *implicit_sized = true;
         assert(*type != NULL);
      }
   }

   static const glsl_type *
   update_interface_members_array(const glsl_type *type,
                                  const glsl_type *new_interface_type)
   {
      const glsl_type *element_type = type->fields.array;
      if (element_type->is_array()) {
         const glsl_type *new_array_type =
            update_interface_members_array(element_type, new_interface_type);
         return glsl_type::get_array_instance(new_array_type, type->length);
      } else {
         return glsl_type::get_array_instance(new_interface_type,
                                              type->length);
      }
   }

   /**
    * Determine whether the given interface type contains unsized arrays (if
    * it doesn't, array_sizing_visitor doesn't need to process it).
    */
   static bool interface_contains_unsized_arrays(const glsl_type *type)
   {
      for (unsigned i = 0; i < type->length; i++) {
         const glsl_type *elem_type = type->fields.structure[i].type;
         if (elem_type->is_unsized_array())
            return true;
      }
      return false;
   }

   /**
    * Create a new interface type based on the given type, with unsized arrays
    * replaced by sized arrays whose size is determined by
    * max_ifc_array_access.
    */
   static const glsl_type *
   resize_interface_members(const glsl_type *type,
                            const int *max_ifc_array_access,
                            bool is_ssbo)
   {
      unsigned num_fields = type->length;
      glsl_struct_field *fields = new glsl_struct_field[num_fields];
      memcpy(fields, type->fields.structure,
             num_fields * sizeof(*fields));
      for (unsigned i = 0; i < num_fields; i++) {
         bool implicit_sized_array = fields[i].implicit_sized_array;
         /* If SSBO last member is unsized array, we don't replace it by a sized
          * array.
          */
         if (is_ssbo && i == (num_fields - 1))
            fixup_type(&fields[i].type, max_ifc_array_access[i],
                       true, &implicit_sized_array);
         else
            fixup_type(&fields[i].type, max_ifc_array_access[i],
                       false, &implicit_sized_array);
         fields[i].implicit_sized_array = implicit_sized_array;
      }
      glsl_interface_packing packing =
         (glsl_interface_packing) type->interface_packing;
      const glsl_type *new_ifc_type =
         glsl_type::get_interface_instance(fields, num_fields,
                                           packing, type->name);
      delete [] fields;
      return new_ifc_type;
   }

   static void fixup_unnamed_interface_type(const void *key, void *data,
                                            void *)
   {
      const glsl_type *ifc_type = (const glsl_type *) key;
      ir_variable **interface_vars = (ir_variable **) data;
      unsigned num_fields = ifc_type->length;
      glsl_struct_field *fields = new glsl_struct_field[num_fields];
      memcpy(fields, ifc_type->fields.structure,
             num_fields * sizeof(*fields));
      bool interface_type_changed = false;
      for (unsigned i = 0; i < num_fields; i++) {
         if (interface_vars[i] != NULL &&
             fields[i].type != interface_vars[i]->type) {
            fields[i].type = interface_vars[i]->type;
            interface_type_changed = true;
         }
      }
      if (!interface_type_changed) {
         delete [] fields;
         return;
      }
      glsl_interface_packing packing =
         (glsl_interface_packing) ifc_type->interface_packing;
      const glsl_type *new_ifc_type =
         glsl_type::get_interface_instance(fields, num_fields, packing,
                                           ifc_type->name);
      delete [] fields;
      for (unsigned i = 0; i < num_fields; i++) {
         if (interface_vars[i] != NULL)
            interface_vars[i]->change_interface_type(new_ifc_type);
      }
   }

   /**
    * Memory context used to allocate the data in \c unnamed_interfaces.
    */
   void *mem_ctx;

   /**
    * Hash table from const glsl_type * to an array of ir_variable *'s
    * pointing to the ir_variables constituting each unnamed interface block.
    */
   hash_table *unnamed_interfaces;
};

/**
 * Check for conflicting xfb_stride default qualifiers and store buffer stride
 * for later use.
 */
static void
link_xfb_stride_layout_qualifiers(struct gl_context *ctx,
                                  struct gl_shader_program *prog,
			          struct gl_shader *linked_shader,
			          struct gl_shader **shader_list,
			          unsigned num_shaders)
{
   for (unsigned i = 0; i < MAX_FEEDBACK_BUFFERS; i++) {
      linked_shader->TransformFeedback.BufferStride[i] = 0;
   }

   for (unsigned i = 0; i < num_shaders; i++) {
      struct gl_shader *shader = shader_list[i];

      for (unsigned j = 0; j < MAX_FEEDBACK_BUFFERS; j++) {
         if (shader->TransformFeedback.BufferStride[j]) {
	    if (linked_shader->TransformFeedback.BufferStride[j] != 0 &&
                shader->TransformFeedback.BufferStride[j] != 0 &&
	        linked_shader->TransformFeedback.BufferStride[j] !=
                   shader->TransformFeedback.BufferStride[j]) {
	       linker_error(prog,
                            "intrastage shaders defined with conflicting "
                            "xfb_stride for buffer %d (%d and %d)\n", j,
                            linked_shader->TransformFeedback.BufferStride[j],
			    shader->TransformFeedback.BufferStride[j]);
	       return;
	    }

            if (shader->TransformFeedback.BufferStride[j])
	       linked_shader->TransformFeedback.BufferStride[j] =
                  shader->TransformFeedback.BufferStride[j];
         }
      }
   }

   for (unsigned j = 0; j < MAX_FEEDBACK_BUFFERS; j++) {
      if (linked_shader->TransformFeedback.BufferStride[j]) {
         prog->TransformFeedback.BufferStride[j] =
            linked_shader->TransformFeedback.BufferStride[j];

         /* We will validate doubles at a later stage */
         if (prog->TransformFeedback.BufferStride[j] % 4) {
            linker_error(prog, "invalid qualifier xfb_stride=%d must be a "
                         "multiple of 4 or if its applied to a type that is "
                         "or contains a double a multiple of 8.",
                         prog->TransformFeedback.BufferStride[j]);
            return;
         }

         if (prog->TransformFeedback.BufferStride[j] / 4 >
             ctx->Const.MaxTransformFeedbackInterleavedComponents) {
            linker_error(prog,
                         "The MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS "
                         "limit has been exceeded.");
                  return;
         }
      }
   }
}

/**
 * Performs the cross-validation of tessellation control shader vertices and
 * layout qualifiers for the attached tessellation control shaders,
 * and propagates them to the linked TCS and linked shader program.
 */
static void
link_tcs_out_layout_qualifiers(struct gl_shader_program *prog,
			      struct gl_shader *linked_shader,
			      struct gl_shader **shader_list,
			      unsigned num_shaders)
{
   linked_shader->TessCtrl.VerticesOut = 0;

   if (linked_shader->Stage != MESA_SHADER_TESS_CTRL)
      return;

   /* From the GLSL 4.0 spec (chapter 4.3.8.2):
    *
    *     "All tessellation control shader layout declarations in a program
    *      must specify the same output patch vertex count.  There must be at
    *      least one layout qualifier specifying an output patch vertex count
    *      in any program containing tessellation control shaders; however,
    *      such a declaration is not required in all tessellation control
    *      shaders."
    */

   for (unsigned i = 0; i < num_shaders; i++) {
      struct gl_shader *shader = shader_list[i];

      if (shader->TessCtrl.VerticesOut != 0) {
	 if (linked_shader->TessCtrl.VerticesOut != 0 &&
	     linked_shader->TessCtrl.VerticesOut != shader->TessCtrl.VerticesOut) {
	    linker_error(prog, "tessellation control shader defined with "
			 "conflicting output vertex count (%d and %d)\n",
			 linked_shader->TessCtrl.VerticesOut,
			 shader->TessCtrl.VerticesOut);
	    return;
	 }
	 linked_shader->TessCtrl.VerticesOut = shader->TessCtrl.VerticesOut;
      }
   }

   /* Just do the intrastage -> interstage propagation right now,
    * since we already know we're in the right type of shader program
    * for doing it.
    */
   if (linked_shader->TessCtrl.VerticesOut == 0) {
      linker_error(prog, "tessellation control shader didn't declare "
		   "vertices out layout qualifier\n");
      return;
   }
   prog->TessCtrl.VerticesOut = linked_shader->TessCtrl.VerticesOut;
}


/**
 * Performs the cross-validation of tessellation evaluation shader
 * primitive type, vertex spacing, ordering and point_mode layout qualifiers
 * for the attached tessellation evaluation shaders, and propagates them
 * to the linked TES and linked shader program.
 */
static void
link_tes_in_layout_qualifiers(struct gl_shader_program *prog,
				struct gl_shader *linked_shader,
				struct gl_shader **shader_list,
				unsigned num_shaders)
{
   linked_shader->TessEval.PrimitiveMode = PRIM_UNKNOWN;
   linked_shader->TessEval.Spacing = 0;
   linked_shader->TessEval.VertexOrder = 0;
   linked_shader->TessEval.PointMode = -1;

   if (linked_shader->Stage != MESA_SHADER_TESS_EVAL)
      return;

   /* From the GLSL 4.0 spec (chapter 4.3.8.1):
    *
    *     "At least one tessellation evaluation shader (compilation unit) in
    *      a program must declare a primitive mode in its input layout.
    *      Declaration vertex spacing, ordering, and point mode identifiers is
    *      optional.  It is not required that all tessellation evaluation
    *      shaders in a program declare a primitive mode.  If spacing or
    *      vertex ordering declarations are omitted, the tessellation
    *      primitive generator will use equal spacing or counter-clockwise
    *      vertex ordering, respectively.  If a point mode declaration is
    *      omitted, the tessellation primitive generator will produce lines or
    *      triangles according to the primitive mode."
    */

   for (unsigned i = 0; i < num_shaders; i++) {
      struct gl_shader *shader = shader_list[i];

      if (shader->TessEval.PrimitiveMode != PRIM_UNKNOWN) {
	 if (linked_shader->TessEval.PrimitiveMode != PRIM_UNKNOWN &&
	     linked_shader->TessEval.PrimitiveMode != shader->TessEval.PrimitiveMode) {
	    linker_error(prog, "tessellation evaluation shader defined with "
			 "conflicting input primitive modes.\n");
	    return;
	 }
	 linked_shader->TessEval.PrimitiveMode = shader->TessEval.PrimitiveMode;
      }

      if (shader->TessEval.Spacing != 0) {
	 if (linked_shader->TessEval.Spacing != 0 &&
	     linked_shader->TessEval.Spacing != shader->TessEval.Spacing) {
	    linker_error(prog, "tessellation evaluation shader defined with "
			 "conflicting vertex spacing.\n");
	    return;
	 }
	 linked_shader->TessEval.Spacing = shader->TessEval.Spacing;
      }

      if (shader->TessEval.VertexOrder != 0) {
	 if (linked_shader->TessEval.VertexOrder != 0 &&
	     linked_shader->TessEval.VertexOrder != shader->TessEval.VertexOrder) {
	    linker_error(prog, "tessellation evaluation shader defined with "
			 "conflicting ordering.\n");
	    return;
	 }
	 linked_shader->TessEval.VertexOrder = shader->TessEval.VertexOrder;
      }

      if (shader->TessEval.PointMode != -1) {
	 if (linked_shader->TessEval.PointMode != -1 &&
	     linked_shader->TessEval.PointMode != shader->TessEval.PointMode) {
	    linker_error(prog, "tessellation evaluation shader defined with "
			 "conflicting point modes.\n");
	    return;
	 }
	 linked_shader->TessEval.PointMode = shader->TessEval.PointMode;
      }

   }

   /* Just do the intrastage -> interstage propagation right now,
    * since we already know we're in the right type of shader program
    * for doing it.
    */
   if (linked_shader->TessEval.PrimitiveMode == PRIM_UNKNOWN) {
      linker_error(prog,
		   "tessellation evaluation shader didn't declare input "
		   "primitive modes.\n");
      return;
   }
   prog->TessEval.PrimitiveMode = linked_shader->TessEval.PrimitiveMode;

   if (linked_shader->TessEval.Spacing == 0)
      linked_shader->TessEval.Spacing = GL_EQUAL;
   prog->TessEval.Spacing = linked_shader->TessEval.Spacing;

   if (linked_shader->TessEval.VertexOrder == 0)
      linked_shader->TessEval.VertexOrder = GL_CCW;
   prog->TessEval.VertexOrder = linked_shader->TessEval.VertexOrder;

   if (linked_shader->TessEval.PointMode == -1)
      linked_shader->TessEval.PointMode = GL_FALSE;
   prog->TessEval.PointMode = linked_shader->TessEval.PointMode;
}


/**
 * Performs the cross-validation of layout qualifiers specified in
 * redeclaration of gl_FragCoord for the attached fragment shaders,
 * and propagates them to the linked FS and linked shader program.
 */
static void
link_fs_input_layout_qualifiers(struct gl_shader_program *prog,
	                        struct gl_shader *linked_shader,
	                        struct gl_shader **shader_list,
	                        unsigned num_shaders)
{
   linked_shader->redeclares_gl_fragcoord = false;
   linked_shader->uses_gl_fragcoord = false;
   linked_shader->origin_upper_left = false;
   linked_shader->pixel_center_integer = false;

   if (linked_shader->Stage != MESA_SHADER_FRAGMENT ||
       (prog->Version < 150 && !prog->ARB_fragment_coord_conventions_enable))
      return;

   for (unsigned i = 0; i < num_shaders; i++) {
      struct gl_shader *shader = shader_list[i];
      /* From the GLSL 1.50 spec, page 39:
       *
       *   "If gl_FragCoord is redeclared in any fragment shader in a program,
       *    it must be redeclared in all the fragment shaders in that program
       *    that have a static use gl_FragCoord."
       */
      if ((linked_shader->redeclares_gl_fragcoord
           && !shader->redeclares_gl_fragcoord
           && shader->uses_gl_fragcoord)
          || (shader->redeclares_gl_fragcoord
              && !linked_shader->redeclares_gl_fragcoord
              && linked_shader->uses_gl_fragcoord)) {
             linker_error(prog, "fragment shader defined with conflicting "
                         "layout qualifiers for gl_FragCoord\n");
      }

      /* From the GLSL 1.50 spec, page 39:
       *
       *   "All redeclarations of gl_FragCoord in all fragment shaders in a
       *    single program must have the same set of qualifiers."
       */
      if (linked_shader->redeclares_gl_fragcoord && shader->redeclares_gl_fragcoord
          && (shader->origin_upper_left != linked_shader->origin_upper_left
          || shader->pixel_center_integer != linked_shader->pixel_center_integer)) {
         linker_error(prog, "fragment shader defined with conflicting "
                      "layout qualifiers for gl_FragCoord\n");
      }

      /* Update the linked shader state.  Note that uses_gl_fragcoord should
       * accumulate the results.  The other values should replace.  If there
       * are multiple redeclarations, all the fields except uses_gl_fragcoord
       * are already known to be the same.
       */
      if (shader->redeclares_gl_fragcoord || shader->uses_gl_fragcoord) {
         linked_shader->redeclares_gl_fragcoord =
            shader->redeclares_gl_fragcoord;
         linked_shader->uses_gl_fragcoord = linked_shader->uses_gl_fragcoord
            || shader->uses_gl_fragcoord;
         linked_shader->origin_upper_left = shader->origin_upper_left;
         linked_shader->pixel_center_integer = shader->pixel_center_integer;
      }

      linked_shader->EarlyFragmentTests |= shader->EarlyFragmentTests;
   }
}

/**
 * Performs the cross-validation of geometry shader max_vertices and
 * primitive type layout qualifiers for the attached geometry shaders,
 * and propagates them to the linked GS and linked shader program.
 */
static void
link_gs_inout_layout_qualifiers(struct gl_shader_program *prog,
				struct gl_shader *linked_shader,
				struct gl_shader **shader_list,
				unsigned num_shaders)
{
   linked_shader->Geom.VerticesOut = -1;
   linked_shader->Geom.Invocations = 0;
   linked_shader->Geom.InputType = PRIM_UNKNOWN;
   linked_shader->Geom.OutputType = PRIM_UNKNOWN;

   /* No in/out qualifiers defined for anything but GLSL 1.50+
    * geometry shaders so far.
    */
   if (linked_shader->Stage != MESA_SHADER_GEOMETRY || prog->Version < 150)
      return;

   /* From the GLSL 1.50 spec, page 46:
    *
    *     "All geometry shader output layout declarations in a program
    *      must declare the same layout and same value for
    *      max_vertices. There must be at least one geometry output
    *      layout declaration somewhere in a program, but not all
    *      geometry shaders (compilation units) are required to
    *      declare it."
    */

   for (unsigned i = 0; i < num_shaders; i++) {
      struct gl_shader *shader = shader_list[i];

      if (shader->Geom.InputType != PRIM_UNKNOWN) {
	 if (linked_shader->Geom.InputType != PRIM_UNKNOWN &&
	     linked_shader->Geom.InputType != shader->Geom.InputType) {
	    linker_error(prog, "geometry shader defined with conflicting "
			 "input types\n");
	    return;
	 }
	 linked_shader->Geom.InputType = shader->Geom.InputType;
      }

      if (shader->Geom.OutputType != PRIM_UNKNOWN) {
	 if (linked_shader->Geom.OutputType != PRIM_UNKNOWN &&
	     linked_shader->Geom.OutputType != shader->Geom.OutputType) {
	    linker_error(prog, "geometry shader defined with conflicting "
			 "output types\n");
	    return;
	 }
	 linked_shader->Geom.OutputType = shader->Geom.OutputType;
      }

      if (shader->Geom.VerticesOut != -1) {
	 if (linked_shader->Geom.VerticesOut != -1 &&
	     linked_shader->Geom.VerticesOut != shader->Geom.VerticesOut) {
	    linker_error(prog, "geometry shader defined with conflicting "
			 "output vertex count (%d and %d)\n",
			 linked_shader->Geom.VerticesOut,
			 shader->Geom.VerticesOut);
	    return;
	 }
	 linked_shader->Geom.VerticesOut = shader->Geom.VerticesOut;
      }

      if (shader->Geom.Invocations != 0) {
	 if (linked_shader->Geom.Invocations != 0 &&
	     linked_shader->Geom.Invocations != shader->Geom.Invocations) {
	    linker_error(prog, "geometry shader defined with conflicting "
			 "invocation count (%d and %d)\n",
			 linked_shader->Geom.Invocations,
			 shader->Geom.Invocations);
	    return;
	 }
	 linked_shader->Geom.Invocations = shader->Geom.Invocations;
      }
   }

   /* Just do the intrastage -> interstage propagation right now,
    * since we already know we're in the right type of shader program
    * for doing it.
    */
   if (linked_shader->Geom.InputType == PRIM_UNKNOWN) {
      linker_error(prog,
		   "geometry shader didn't declare primitive input type\n");
      return;
   }
   prog->Geom.InputType = linked_shader->Geom.InputType;

   if (linked_shader->Geom.OutputType == PRIM_UNKNOWN) {
      linker_error(prog,
		   "geometry shader didn't declare primitive output type\n");
      return;
   }
   prog->Geom.OutputType = linked_shader->Geom.OutputType;

   if (linked_shader->Geom.VerticesOut == -1) {
      linker_error(prog,
		   "geometry shader didn't declare max_vertices\n");
      return;
   }
   prog->Geom.VerticesOut = linked_shader->Geom.VerticesOut;

   if (linked_shader->Geom.Invocations == 0)
      linked_shader->Geom.Invocations = 1;

   prog->Geom.Invocations = linked_shader->Geom.Invocations;
}


/**
 * Perform cross-validation of compute shader local_size_{x,y,z} layout
 * qualifiers for the attached compute shaders, and propagate them to the
 * linked CS and linked shader program.
 */
static void
link_cs_input_layout_qualifiers(struct gl_shader_program *prog,
                                struct gl_shader *linked_shader,
                                struct gl_shader **shader_list,
                                unsigned num_shaders)
{
   for (int i = 0; i < 3; i++)
      linked_shader->Comp.LocalSize[i] = 0;

   /* This function is called for all shader stages, but it only has an effect
    * for compute shaders.
    */
   if (linked_shader->Stage != MESA_SHADER_COMPUTE)
      return;

   /* From the ARB_compute_shader spec, in the section describing local size
    * declarations:
    *
    *     If multiple compute shaders attached to a single program object
    *     declare local work-group size, the declarations must be identical;
    *     otherwise a link-time error results. Furthermore, if a program
    *     object contains any compute shaders, at least one must contain an
    *     input layout qualifier specifying the local work sizes of the
    *     program, or a link-time error will occur.
    */
   for (unsigned sh = 0; sh < num_shaders; sh++) {
      struct gl_shader *shader = shader_list[sh];

      if (shader->Comp.LocalSize[0] != 0) {
         if (linked_shader->Comp.LocalSize[0] != 0) {
            for (int i = 0; i < 3; i++) {
               if (linked_shader->Comp.LocalSize[i] !=
                   shader->Comp.LocalSize[i]) {
                  linker_error(prog, "compute shader defined with conflicting "
                               "local sizes\n");
                  return;
               }
            }
         }
         for (int i = 0; i < 3; i++)
            linked_shader->Comp.LocalSize[i] = shader->Comp.LocalSize[i];
      }
   }

   /* Just do the intrastage -> interstage propagation right now,
    * since we already know we're in the right type of shader program
    * for doing it.
    */
   if (linked_shader->Comp.LocalSize[0] == 0) {
      linker_error(prog, "compute shader didn't declare local size\n");
      return;
   }
   for (int i = 0; i < 3; i++)
      prog->Comp.LocalSize[i] = linked_shader->Comp.LocalSize[i];
}


/**
 * Combine a group of shaders for a single stage to generate a linked shader
 *
 * \note
 * If this function is supplied a single shader, it is cloned, and the new
 * shader is returned.
 */
static struct gl_shader *
link_intrastage_shaders(void *mem_ctx,
			struct gl_context *ctx,
			struct gl_shader_program *prog,
			struct gl_shader **shader_list,
			unsigned num_shaders)
{
   struct gl_uniform_block *ubo_blocks = NULL;
   struct gl_uniform_block *ssbo_blocks = NULL;
   unsigned num_ubo_blocks = 0;
   unsigned num_ssbo_blocks = 0;

   /* Check that global variables defined in multiple shaders are consistent.
    */
   cross_validate_globals(prog, shader_list, num_shaders, false);
   if (!prog->LinkStatus)
      return NULL;

   /* Check that interface blocks defined in multiple shaders are consistent.
    */
   validate_intrastage_interface_blocks(prog, (const gl_shader **)shader_list,
                                        num_shaders);
   if (!prog->LinkStatus)
      return NULL;

   /* Check that there is only a single definition of each function signature
    * across all shaders.
    */
   for (unsigned i = 0; i < (num_shaders - 1); i++) {
      foreach_in_list(ir_instruction, node, shader_list[i]->ir) {
	 ir_function *const f = node->as_function();

	 if (f == NULL)
	    continue;

	 for (unsigned j = i + 1; j < num_shaders; j++) {
	    ir_function *const other =
	       shader_list[j]->symbols->get_function(f->name);

	    /* If the other shader has no function (and therefore no function
	     * signatures) with the same name, skip to the next shader.
	     */
	    if (other == NULL)
	       continue;

	    foreach_in_list(ir_function_signature, sig, &f->signatures) {
	       if (!sig->is_defined || sig->is_builtin())
		  continue;

	       ir_function_signature *other_sig =
		  other->exact_matching_signature(NULL, &sig->parameters);

	       if ((other_sig != NULL) && other_sig->is_defined
		   && !other_sig->is_builtin()) {
		  linker_error(prog, "function `%s' is multiply defined\n",
			       f->name);
		  return NULL;
	       }
	    }
	 }
      }
   }

   /* Find the shader that defines main, and make a clone of it.
    *
    * Starting with the clone, search for undefined references.  If one is
    * found, find the shader that defines it.  Clone the reference and add
    * it to the shader.  Repeat until there are no undefined references or
    * until a reference cannot be resolved.
    */
   gl_shader *main = NULL;
   for (unsigned i = 0; i < num_shaders; i++) {
      if (_mesa_get_main_function_signature(shader_list[i]) != NULL) {
	 main = shader_list[i];
	 break;
      }
   }

   if (main == NULL) {
      linker_error(prog, "%s shader lacks `main'\n",
		   _mesa_shader_stage_to_string(shader_list[0]->Stage));
      return NULL;
   }

   gl_shader *linked = ctx->Driver.NewShader(NULL, 0, shader_list[0]->Stage);
   linked->ir = new(linked) exec_list;
   clone_ir_list(mem_ctx, linked->ir, main->ir);

   link_fs_input_layout_qualifiers(prog, linked, shader_list, num_shaders);
   link_tcs_out_layout_qualifiers(prog, linked, shader_list, num_shaders);
   link_tes_in_layout_qualifiers(prog, linked, shader_list, num_shaders);
   link_gs_inout_layout_qualifiers(prog, linked, shader_list, num_shaders);
   link_cs_input_layout_qualifiers(prog, linked, shader_list, num_shaders);
   link_xfb_stride_layout_qualifiers(ctx, prog, linked, shader_list,
                                     num_shaders);

   populate_symbol_table(linked);

   /* The pointer to the main function in the final linked shader (i.e., the
    * copy of the original shader that contained the main function).
    */
   ir_function_signature *const main_sig =
      _mesa_get_main_function_signature(linked);

   /* Move any instructions other than variable declarations or function
    * declarations into main.
    */
   exec_node *insertion_point =
      move_non_declarations(linked->ir, (exec_node *) &main_sig->body, false,
			    linked);

   for (unsigned i = 0; i < num_shaders; i++) {
      if (shader_list[i] == main)
	 continue;

      insertion_point = move_non_declarations(shader_list[i]->ir,
					      insertion_point, true, linked);
   }

   /* Check if any shader needs built-in functions. */
   bool need_builtins = false;
   for (unsigned i = 0; i < num_shaders; i++) {
      if (shader_list[i]->uses_builtin_functions) {
         need_builtins = true;
         break;
      }
   }

   bool ok;
   if (need_builtins) {
      /* Make a temporary array one larger than shader_list, which will hold
       * the built-in function shader as well.
       */
      gl_shader **linking_shaders = (gl_shader **)
         calloc(num_shaders + 1, sizeof(gl_shader *));

      ok = linking_shaders != NULL;

      if (ok) {
         memcpy(linking_shaders, shader_list, num_shaders * sizeof(gl_shader *));
         _mesa_glsl_initialize_builtin_functions();
         linking_shaders[num_shaders] = _mesa_glsl_get_builtin_function_shader();

         ok = link_function_calls(prog, linked, linking_shaders, num_shaders + 1);

         free(linking_shaders);
      } else {
         _mesa_error_no_memory(__func__);
      }
   } else {
      ok = link_function_calls(prog, linked, shader_list, num_shaders);
   }


   if (!ok) {
      _mesa_delete_shader(ctx, linked);
      return NULL;
   }

   /* Make a pass over all variable declarations to ensure that arrays with
    * unspecified sizes have a size specified.  The size is inferred from the
    * max_array_access field.
    */
   array_sizing_visitor v;
   v.run(linked->ir);
   v.fixup_unnamed_interface_types();

   /* Link up uniform blocks defined within this stage. */
   link_uniform_blocks(mem_ctx, ctx, prog, &linked, 1,
                       &ubo_blocks, &num_ubo_blocks, &ssbo_blocks,
                       &num_ssbo_blocks);

   if (!prog->LinkStatus) {
      _mesa_delete_shader(ctx, linked);
      return NULL;
   }

   /* Copy ubo blocks to linked shader list */
   linked->UniformBlocks =
      ralloc_array(linked, gl_uniform_block *, num_ubo_blocks);
   ralloc_steal(linked, ubo_blocks);
   for (unsigned i = 0; i < num_ubo_blocks; i++) {
      linked->UniformBlocks[i] = &ubo_blocks[i];
   }
   linked->NumUniformBlocks = num_ubo_blocks;

   /* Copy ssbo blocks to linked shader list */
   linked->ShaderStorageBlocks =
      ralloc_array(linked, gl_uniform_block *, num_ssbo_blocks);
   ralloc_steal(linked, ssbo_blocks);
   for (unsigned i = 0; i < num_ssbo_blocks; i++) {
      linked->ShaderStorageBlocks[i] = &ssbo_blocks[i];
   }
   linked->NumShaderStorageBlocks = num_ssbo_blocks;

   /* At this point linked should contain all of the linked IR, so
    * validate it to make sure nothing went wrong.
    */
   validate_ir_tree(linked->ir);

   /* Set the size of geometry shader input arrays */
   if (linked->Stage == MESA_SHADER_GEOMETRY) {
      unsigned num_vertices = vertices_per_prim(prog->Geom.InputType);
      geom_array_resize_visitor input_resize_visitor(num_vertices, prog);
      foreach_in_list(ir_instruction, ir, linked->ir) {
         ir->accept(&input_resize_visitor);
      }
   }

   if (ctx->Const.VertexID_is_zero_based)
      lower_vertex_id(linked);

   /* Validate correct usage of barrier() in the tess control shader */
   if (linked->Stage == MESA_SHADER_TESS_CTRL) {
      barrier_use_visitor visitor(prog);
      foreach_in_list(ir_instruction, ir, linked->ir) {
         ir->accept(&visitor);
      }
   }

   return linked;
}

/**
 * Update the sizes of linked shader uniform arrays to the maximum
 * array index used.
 *
 * From page 81 (page 95 of the PDF) of the OpenGL 2.1 spec:
 *
 *     If one or more elements of an array are active,
 *     GetActiveUniform will return the name of the array in name,
 *     subject to the restrictions listed above. The type of the array
 *     is returned in type. The size parameter contains the highest
 *     array element index used, plus one. The compiler or linker
 *     determines the highest index used.  There will be only one
 *     active uniform reported by the GL per uniform array.

 */
static void
update_array_sizes(struct gl_shader_program *prog)
{
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
	 if (prog->_LinkedShaders[i] == NULL)
	    continue;

      foreach_in_list(ir_instruction, node, prog->_LinkedShaders[i]->ir) {
	 ir_variable *const var = node->as_variable();

	 if ((var == NULL) || (var->data.mode != ir_var_uniform) ||
	     !var->type->is_array())
	    continue;

	 /* GL_ARB_uniform_buffer_object says that std140 uniforms
	  * will not be eliminated.  Since we always do std140, just
	  * don't resize arrays in UBOs.
          *
          * Atomic counters are supposed to get deterministic
          * locations assigned based on the declaration ordering and
          * sizes, array compaction would mess that up.
          *
          * Subroutine uniforms are not removed.
	  */
	 if (var->is_in_buffer_block() || var->type->contains_atomic() ||
	     var->type->contains_subroutine() || var->constant_initializer)
	    continue;

	 int size = var->data.max_array_access;
	 for (unsigned j = 0; j < MESA_SHADER_STAGES; j++) {
	       if (prog->_LinkedShaders[j] == NULL)
		  continue;

	    foreach_in_list(ir_instruction, node2, prog->_LinkedShaders[j]->ir) {
	       ir_variable *other_var = node2->as_variable();
	       if (!other_var)
		  continue;

	       if (strcmp(var->name, other_var->name) == 0 &&
		   other_var->data.max_array_access > size) {
		  size = other_var->data.max_array_access;
	       }
	    }
	 }

	 if (size + 1 != (int)var->type->length) {
	    /* If this is a built-in uniform (i.e., it's backed by some
	     * fixed-function state), adjust the number of state slots to
	     * match the new array size.  The number of slots per array entry
	     * is not known.  It seems safe to assume that the total number of
	     * slots is an integer multiple of the number of array elements.
	     * Determine the number of slots per array element by dividing by
	     * the old (total) size.
	     */
            const unsigned num_slots = var->get_num_state_slots();
	    if (num_slots > 0) {
	       var->set_num_state_slots((size + 1)
                                        * (num_slots / var->type->length));
	    }

	    var->type = glsl_type::get_array_instance(var->type->fields.array,
						      size + 1);
	    /* FINISHME: We should update the types of array
	     * dereferences of this variable now.
	     */
	 }
      }
   }
}

/**
 * Resize tessellation evaluation per-vertex inputs to the size of
 * tessellation control per-vertex outputs.
 */
static void
resize_tes_inputs(struct gl_context *ctx,
                  struct gl_shader_program *prog)
{
   if (prog->_LinkedShaders[MESA_SHADER_TESS_EVAL] == NULL)
      return;

   gl_shader *const tcs = prog->_LinkedShaders[MESA_SHADER_TESS_CTRL];
   gl_shader *const tes = prog->_LinkedShaders[MESA_SHADER_TESS_EVAL];

   /* If no control shader is present, then the TES inputs are statically
    * sized to MaxPatchVertices; the actual size of the arrays won't be
    * known until draw time.
    */
   const int num_vertices = tcs
      ? tcs->TessCtrl.VerticesOut
      : ctx->Const.MaxPatchVertices;

   tess_eval_array_resize_visitor input_resize_visitor(num_vertices, prog);
   foreach_in_list(ir_instruction, ir, tes->ir) {
      ir->accept(&input_resize_visitor);
   }

   if (tcs || ctx->Const.LowerTESPatchVerticesIn) {
      /* Convert the gl_PatchVerticesIn system value into a constant, since
       * the value is known at this point.
       */
      foreach_in_list(ir_instruction, ir, tes->ir) {
         ir_variable *var = ir->as_variable();
         if (var && var->data.mode == ir_var_system_value &&
             var->data.location == SYSTEM_VALUE_VERTICES_IN) {
            void *mem_ctx = ralloc_parent(var);
            var->data.location = 0;
            var->data.explicit_location = false;
            if (tcs) {
               var->data.mode = ir_var_auto;
               var->constant_value = new(mem_ctx) ir_constant(num_vertices);
            } else {
               var->data.mode = ir_var_uniform;
               var->data.how_declared = ir_var_hidden;
               var->allocate_state_slots(1);
               ir_state_slot *slot0 = &var->get_state_slots()[0];
               slot0->swizzle = SWIZZLE_XXXX;
               slot0->tokens[0] = STATE_INTERNAL;
               slot0->tokens[1] = STATE_TES_PATCH_VERTICES_IN;
               for (int i = 2; i < STATE_LENGTH; i++)
                  slot0->tokens[i] = 0;
            }
         }
      }
   }
}

/**
 * Find a contiguous set of available bits in a bitmask.
 *
 * \param used_mask     Bits representing used (1) and unused (0) locations
 * \param needed_count  Number of contiguous bits needed.
 *
 * \return
 * Base location of the available bits on success or -1 on failure.
 */
int
find_available_slots(unsigned used_mask, unsigned needed_count)
{
   unsigned needed_mask = (1 << needed_count) - 1;
   const int max_bit_to_test = (8 * sizeof(used_mask)) - needed_count;

   /* The comparison to 32 is redundant, but without it GCC emits "warning:
    * cannot optimize possibly infinite loops" for the loop below.
    */
   if ((needed_count == 0) || (max_bit_to_test < 0) || (max_bit_to_test > 32))
      return -1;

   for (int i = 0; i <= max_bit_to_test; i++) {
      if ((needed_mask & ~used_mask) == needed_mask)
	 return i;

      needed_mask <<= 1;
   }

   return -1;
}


/**
 * Assign locations for either VS inputs or FS outputs
 *
 * \param prog          Shader program whose variables need locations assigned
 * \param constants     Driver specific constant values for the program.
 * \param target_index  Selector for the program target to receive location
 *                      assignmnets.  Must be either \c MESA_SHADER_VERTEX or
 *                      \c MESA_SHADER_FRAGMENT.
 *
 * \return
 * If locations are successfully assigned, true is returned.  Otherwise an
 * error is emitted to the shader link log and false is returned.
 */
bool
assign_attribute_or_color_locations(gl_shader_program *prog,
                                    struct gl_constants *constants,
                                    unsigned target_index)
{
   /* Maximum number of generic locations.  This corresponds to either the
    * maximum number of draw buffers or the maximum number of generic
    * attributes.
    */
   unsigned max_index = (target_index == MESA_SHADER_VERTEX) ?
      constants->Program[target_index].MaxAttribs :
      MAX2(constants->MaxDrawBuffers, constants->MaxDualSourceDrawBuffers);

   /* Mark invalid locations as being used.
    */
   unsigned used_locations = (max_index >= 32)
      ? ~0 : ~((1 << max_index) - 1);
   unsigned double_storage_locations = 0;

   assert((target_index == MESA_SHADER_VERTEX)
	  || (target_index == MESA_SHADER_FRAGMENT));

   gl_shader *const sh = prog->_LinkedShaders[target_index];
   if (sh == NULL)
      return true;

   /* Operate in a total of four passes.
    *
    * 1. Invalidate the location assignments for all vertex shader inputs.
    *
    * 2. Assign locations for inputs that have user-defined (via
    *    glBindVertexAttribLocation) locations and outputs that have
    *    user-defined locations (via glBindFragDataLocation).
    *
    * 3. Sort the attributes without assigned locations by number of slots
    *    required in decreasing order.  Fragmentation caused by attribute
    *    locations assigned by the application may prevent large attributes
    *    from having enough contiguous space.
    *
    * 4. Assign locations to any inputs without assigned locations.
    */

   const int generic_base = (target_index == MESA_SHADER_VERTEX)
      ? (int) VERT_ATTRIB_GENERIC0 : (int) FRAG_RESULT_DATA0;

   const enum ir_variable_mode direction =
      (target_index == MESA_SHADER_VERTEX)
      ? ir_var_shader_in : ir_var_shader_out;


   /* Temporary storage for the set of attributes that need locations assigned.
    */
   struct temp_attr {
      unsigned slots;
      ir_variable *var;

      /* Used below in the call to qsort. */
      static int compare(const void *a, const void *b)
      {
	 const temp_attr *const l = (const temp_attr *) a;
	 const temp_attr *const r = (const temp_attr *) b;

	 /* Reversed because we want a descending order sort below. */
	 return r->slots - l->slots;
      }
   } to_assign[32];
   assert(max_index <= 32);

   /* Temporary array for the set of attributes that have locations assigned.
    */
   ir_variable *assigned[16];

   unsigned num_attr = 0;
   unsigned assigned_attr = 0;

   foreach_in_list(ir_instruction, node, sh->ir) {
      ir_variable *const var = node->as_variable();

      if ((var == NULL) || (var->data.mode != (unsigned) direction))
	 continue;

      if (var->data.explicit_location) {
         var->data.is_unmatched_generic_inout = 0;
	 if ((var->data.location >= (int)(max_index + generic_base))
	     || (var->data.location < 0)) {
	    linker_error(prog,
			 "invalid explicit location %d specified for `%s'\n",
			 (var->data.location < 0)
			 ? var->data.location
                         : var->data.location - generic_base,
			 var->name);
	    return false;
	 }
      } else if (target_index == MESA_SHADER_VERTEX) {
	 unsigned binding;

	 if (prog->AttributeBindings->get(binding, var->name)) {
	    assert(binding >= VERT_ATTRIB_GENERIC0);
	    var->data.location = binding;
            var->data.is_unmatched_generic_inout = 0;
	 }
      } else if (target_index == MESA_SHADER_FRAGMENT) {
	 unsigned binding;
	 unsigned index;

	 if (prog->FragDataBindings->get(binding, var->name)) {
	    assert(binding >= FRAG_RESULT_DATA0);
	    var->data.location = binding;
            var->data.is_unmatched_generic_inout = 0;

	    if (prog->FragDataIndexBindings->get(index, var->name)) {
	       var->data.index = index;
	    }
	 }
      }

      /* From GL4.5 core spec, section 15.2 (Shader Execution):
       *
       *     "Output binding assignments will cause LinkProgram to fail:
       *     ...
       *     If the program has an active output assigned to a location greater
       *     than or equal to the value of MAX_DUAL_SOURCE_DRAW_BUFFERS and has
       *     an active output assigned an index greater than or equal to one;"
       */
      if (target_index == MESA_SHADER_FRAGMENT && var->data.index >= 1 &&
          var->data.location - generic_base >=
          (int) constants->MaxDualSourceDrawBuffers) {
         linker_error(prog,
                      "output location %d >= GL_MAX_DUAL_SOURCE_DRAW_BUFFERS "
                      "with index %u for %s\n",
                      var->data.location - generic_base, var->data.index,
                      var->name);
         return false;
      }

      const unsigned slots = var->type->count_attribute_slots(target_index == MESA_SHADER_VERTEX);

      /* If the variable is not a built-in and has a location statically
       * assigned in the shader (presumably via a layout qualifier), make sure
       * that it doesn't collide with other assigned locations.  Otherwise,
       * add it to the list of variables that need linker-assigned locations.
       */
      if (var->data.location != -1) {
	 if (var->data.location >= generic_base && var->data.index < 1) {
	    /* From page 61 of the OpenGL 4.0 spec:
	     *
	     *     "LinkProgram will fail if the attribute bindings assigned
	     *     by BindAttribLocation do not leave not enough space to
	     *     assign a location for an active matrix attribute or an
	     *     active attribute array, both of which require multiple
	     *     contiguous generic attributes."
	     *
	     * I think above text prohibits the aliasing of explicit and
	     * automatic assignments. But, aliasing is allowed in manual
	     * assignments of attribute locations. See below comments for
	     * the details.
	     *
	     * From OpenGL 4.0 spec, page 61:
	     *
	     *     "It is possible for an application to bind more than one
	     *     attribute name to the same location. This is referred to as
	     *     aliasing. This will only work if only one of the aliased
	     *     attributes is active in the executable program, or if no
	     *     path through the shader consumes more than one attribute of
	     *     a set of attributes aliased to the same location. A link
	     *     error can occur if the linker determines that every path
	     *     through the shader consumes multiple aliased attributes,
	     *     but implementations are not required to generate an error
	     *     in this case."
	     *
	     * From GLSL 4.30 spec, page 54:
	     *
	     *    "A program will fail to link if any two non-vertex shader
	     *     input variables are assigned to the same location. For
	     *     vertex shaders, multiple input variables may be assigned
	     *     to the same location using either layout qualifiers or via
	     *     the OpenGL API. However, such aliasing is intended only to
	     *     support vertex shaders where each execution path accesses
	     *     at most one input per each location. Implementations are
	     *     permitted, but not required, to generate link-time errors
	     *     if they detect that every path through the vertex shader
	     *     executable accesses multiple inputs assigned to any single
	     *     location. For all shader types, a program will fail to link
	     *     if explicit location assignments leave the linker unable
	     *     to find space for other variables without explicit
	     *     assignments."
	     *
	     * From OpenGL ES 3.0 spec, page 56:
	     *
	     *    "Binding more than one attribute name to the same location
	     *     is referred to as aliasing, and is not permitted in OpenGL
	     *     ES Shading Language 3.00 vertex shaders. LinkProgram will
	     *     fail when this condition exists. However, aliasing is
	     *     possible in OpenGL ES Shading Language 1.00 vertex shaders.
	     *     This will only work if only one of the aliased attributes
	     *     is active in the executable program, or if no path through
	     *     the shader consumes more than one attribute of a set of
	     *     attributes aliased to the same location. A link error can
	     *     occur if the linker determines that every path through the
	     *     shader consumes multiple aliased attributes, but implemen-
	     *     tations are not required to generate an error in this case."
	     *
	     * After looking at above references from OpenGL, OpenGL ES and
	     * GLSL specifications, we allow aliasing of vertex input variables
	     * in: OpenGL 2.0 (and above) and OpenGL ES 2.0.
	     *
	     * NOTE: This is not required by the spec but its worth mentioning
	     * here that we're not doing anything to make sure that no path
	     * through the vertex shader executable accesses multiple inputs
	     * assigned to any single location.
	     */

	    /* Mask representing the contiguous slots that will be used by
	     * this attribute.
	     */
	    const unsigned attr = var->data.location - generic_base;
	    const unsigned use_mask = (1 << slots) - 1;
            const char *const string = (target_index == MESA_SHADER_VERTEX)
               ? "vertex shader input" : "fragment shader output";

            /* Generate a link error if the requested locations for this
             * attribute exceed the maximum allowed attribute location.
             */
            if (attr + slots > max_index) {
               linker_error(prog,
                           "insufficient contiguous locations "
                           "available for %s `%s' %d %d %d\n", string,
                           var->name, used_locations, use_mask, attr);
               return false;
            }

	    /* Generate a link error if the set of bits requested for this
	     * attribute overlaps any previously allocated bits.
	     */
	    if ((~(use_mask << attr) & used_locations) != used_locations) {
               if (target_index == MESA_SHADER_FRAGMENT && !prog->IsES) {
                  /* From section 4.4.2 (Output Layout Qualifiers) of the GLSL
                   * 4.40 spec:
                   *
                   *    "Additionally, for fragment shader outputs, if two
                   *    variables are placed within the same location, they
                   *    must have the same underlying type (floating-point or
                   *    integer). No component aliasing of output variables or
                   *    members is allowed.
                   */
                  for (unsigned i = 0; i < assigned_attr; i++) {
                     unsigned assigned_slots =
                        assigned[i]->type->count_attribute_slots(false);
	             unsigned assig_attr =
                        assigned[i]->data.location - generic_base;
	             unsigned assigned_use_mask = (1 << assigned_slots) - 1;

                     if ((assigned_use_mask << assig_attr) &
                         (use_mask << attr)) {

                        const glsl_type *assigned_type =
                           assigned[i]->type->without_array();
                        const glsl_type *type = var->type->without_array();
                        if (assigned_type->base_type != type->base_type) {
                           linker_error(prog, "types do not match for aliased"
                                        " %ss %s and %s\n", string,
                                        assigned[i]->name, var->name);
                           return false;
                        }

                        unsigned assigned_component_mask =
                           ((1 << assigned_type->vector_elements) - 1) <<
                           assigned[i]->data.location_frac;
                        unsigned component_mask =
                           ((1 << type->vector_elements) - 1) <<
                           var->data.location_frac;
                        if (assigned_component_mask & component_mask) {
                           linker_error(prog, "overlapping component is "
                                        "assigned to %ss %s and %s "
                                        "(component=%d)\n",
                                        string, assigned[i]->name, var->name,
                                        var->data.location_frac);
                           return false;
                        }
                     }
                  }
               } else if (target_index == MESA_SHADER_FRAGMENT ||
                          (prog->IsES && prog->Version >= 300)) {
                  linker_error(prog, "overlapping location is assigned "
                               "to %s `%s' %d %d %d\n", string, var->name,
                               used_locations, use_mask, attr);
                  return false;
               } else {
                  linker_warning(prog, "overlapping location is assigned "
                                 "to %s `%s' %d %d %d\n", string, var->name,
                                 used_locations, use_mask, attr);
               }
	    }

	    used_locations |= (use_mask << attr);

            /* From the GL 4.5 core spec, section 11.1.1 (Vertex Attributes):
             *
             * "A program with more than the value of MAX_VERTEX_ATTRIBS
             *  active attribute variables may fail to link, unless
             *  device-dependent optimizations are able to make the program
             *  fit within available hardware resources. For the purposes
             *  of this test, attribute variables of the type dvec3, dvec4,
             *  dmat2x3, dmat2x4, dmat3, dmat3x4, dmat4x3, and dmat4 may
             *  count as consuming twice as many attributes as equivalent
             *  single-precision types. While these types use the same number
             *  of generic attributes as their single-precision equivalents,
             *  implementations are permitted to consume two single-precision
             *  vectors of internal storage for each three- or four-component
             *  double-precision vector."
             *
             * Mark this attribute slot as taking up twice as much space
             * so we can count it properly against limits.  According to
             * issue (3) of the GL_ARB_vertex_attrib_64bit behavior, this
             * is optional behavior, but it seems preferable.
             */
            if (var->type->without_array()->is_dual_slot())
               double_storage_locations |= (use_mask << attr);
	 }

         assigned[assigned_attr] = var;
         assigned_attr++;

	 continue;
      }

      if (num_attr >= max_index) {
         linker_error(prog, "too many %s (max %u)",
                      target_index == MESA_SHADER_VERTEX ?
                      "vertex shader inputs" : "fragment shader outputs",
                      max_index);
         return false;
      }
      to_assign[num_attr].slots = slots;
      to_assign[num_attr].var = var;
      num_attr++;
   }

   if (target_index == MESA_SHADER_VERTEX) {
      unsigned total_attribs_size =
         _mesa_bitcount(used_locations & ((1 << max_index) - 1)) +
         _mesa_bitcount(double_storage_locations);
      if (total_attribs_size > max_index) {
	 linker_error(prog,
		      "attempt to use %d vertex attribute slots only %d available ",
		      total_attribs_size, max_index);
	 return false;
      }
   }

   /* If all of the attributes were assigned locations by the application (or
    * are built-in attributes with fixed locations), return early.  This should
    * be the common case.
    */
   if (num_attr == 0)
      return true;

   qsort(to_assign, num_attr, sizeof(to_assign[0]), temp_attr::compare);

   if (target_index == MESA_SHADER_VERTEX) {
      /* VERT_ATTRIB_GENERIC0 is a pseudo-alias for VERT_ATTRIB_POS.  It can
       * only be explicitly assigned by via glBindAttribLocation.  Mark it as
       * reserved to prevent it from being automatically allocated below.
       */
      find_deref_visitor find("gl_Vertex");
      find.run(sh->ir);
      if (find.variable_found())
	 used_locations |= (1 << 0);
   }

   for (unsigned i = 0; i < num_attr; i++) {
      /* Mask representing the contiguous slots that will be used by this
       * attribute.
       */
      const unsigned use_mask = (1 << to_assign[i].slots) - 1;

      int location = find_available_slots(used_locations, to_assign[i].slots);

      if (location < 0) {
	 const char *const string = (target_index == MESA_SHADER_VERTEX)
	    ? "vertex shader input" : "fragment shader output";

	 linker_error(prog,
		      "insufficient contiguous locations "
		      "available for %s `%s'\n",
		      string, to_assign[i].var->name);
	 return false;
      }

      to_assign[i].var->data.location = generic_base + location;
      to_assign[i].var->data.is_unmatched_generic_inout = 0;
      used_locations |= (use_mask << location);

      if (to_assign[i].var->type->without_array()->is_dual_slot())
         double_storage_locations |= (use_mask << location);
   }

   /* Now that we have all the locations, from the GL 4.5 core spec, section
    * 11.1.1 (Vertex Attributes), dvec3, dvec4, dmat2x3, dmat2x4, dmat3,
    * dmat3x4, dmat4x3, and dmat4 count as consuming twice as many attributes
    * as equivalent single-precision types.
    */
   if (target_index == MESA_SHADER_VERTEX) {
      unsigned total_attribs_size =
         _mesa_bitcount(used_locations & ((1 << max_index) - 1)) +
         _mesa_bitcount(double_storage_locations);
      if (total_attribs_size > max_index) {
	 linker_error(prog,
		      "attempt to use %d vertex attribute slots only %d available ",
		      total_attribs_size, max_index);
	 return false;
      }
   }

   return true;
}

/**
 * Match explicit locations of outputs to inputs and deactivate the
 * unmatch flag if found so we don't optimise them away.
 */
static void
match_explicit_outputs_to_inputs(gl_shader *producer,
                                 gl_shader *consumer)
{
   glsl_symbol_table parameters;
   ir_variable *explicit_locations[MAX_VARYINGS_INCL_PATCH][4] =
      { {NULL, NULL} };

   /* Find all shader outputs in the "producer" stage.
    */
   foreach_in_list(ir_instruction, node, producer->ir) {
      ir_variable *const var = node->as_variable();

      if ((var == NULL) || (var->data.mode != ir_var_shader_out))
         continue;

      if (var->data.explicit_location &&
          var->data.location >= VARYING_SLOT_VAR0) {
         const unsigned idx = var->data.location - VARYING_SLOT_VAR0;
         if (explicit_locations[idx][var->data.location_frac] == NULL)
            explicit_locations[idx][var->data.location_frac] = var;
      }
   }

   /* Match inputs to outputs */
   foreach_in_list(ir_instruction, node, consumer->ir) {
      ir_variable *const input = node->as_variable();

      if ((input == NULL) || (input->data.mode != ir_var_shader_in))
         continue;

      ir_variable *output = NULL;
      if (input->data.explicit_location
          && input->data.location >= VARYING_SLOT_VAR0) {
         output = explicit_locations[input->data.location - VARYING_SLOT_VAR0]
            [input->data.location_frac];

         if (output != NULL){
            input->data.is_unmatched_generic_inout = 0;
            output->data.is_unmatched_generic_inout = 0;
         }
      }
   }
}

/**
 * Store the gl_FragDepth layout in the gl_shader_program struct.
 */
static void
store_fragdepth_layout(struct gl_shader_program *prog)
{
   if (prog->_LinkedShaders[MESA_SHADER_FRAGMENT] == NULL) {
      return;
   }

   struct exec_list *ir = prog->_LinkedShaders[MESA_SHADER_FRAGMENT]->ir;

   /* We don't look up the gl_FragDepth symbol directly because if
    * gl_FragDepth is not used in the shader, it's removed from the IR.
    * However, the symbol won't be removed from the symbol table.
    *
    * We're only interested in the cases where the variable is NOT removed
    * from the IR.
    */
   foreach_in_list(ir_instruction, node, ir) {
      ir_variable *const var = node->as_variable();

      if (var == NULL || var->data.mode != ir_var_shader_out) {
         continue;
      }

      if (strcmp(var->name, "gl_FragDepth") == 0) {
         switch (var->data.depth_layout) {
         case ir_depth_layout_none:
            prog->FragDepthLayout = FRAG_DEPTH_LAYOUT_NONE;
            return;
         case ir_depth_layout_any:
            prog->FragDepthLayout = FRAG_DEPTH_LAYOUT_ANY;
            return;
         case ir_depth_layout_greater:
            prog->FragDepthLayout = FRAG_DEPTH_LAYOUT_GREATER;
            return;
         case ir_depth_layout_less:
            prog->FragDepthLayout = FRAG_DEPTH_LAYOUT_LESS;
            return;
         case ir_depth_layout_unchanged:
            prog->FragDepthLayout = FRAG_DEPTH_LAYOUT_UNCHANGED;
            return;
         default:
            assert(0);
            return;
         }
      }
   }
}

/**
 * Validate the resources used by a program versus the implementation limits
 */
static void
check_resources(struct gl_context *ctx, struct gl_shader_program *prog)
{
   unsigned total_uniform_blocks = 0;
   unsigned total_shader_storage_blocks = 0;

   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      struct gl_shader *sh = prog->_LinkedShaders[i];

      if (sh == NULL)
	 continue;

      if (sh->num_samplers > ctx->Const.Program[i].MaxTextureImageUnits) {
	 linker_error(prog, "Too many %s shader texture samplers\n",
		      _mesa_shader_stage_to_string(i));
      }

      if (sh->num_uniform_components >
          ctx->Const.Program[i].MaxUniformComponents) {
         if (ctx->Const.GLSLSkipStrictMaxUniformLimitCheck) {
            linker_warning(prog, "Too many %s shader default uniform block "
                           "components, but the driver will try to optimize "
                           "them out; this is non-portable out-of-spec "
			   "behavior\n",
                           _mesa_shader_stage_to_string(i));
         } else {
            linker_error(prog, "Too many %s shader default uniform block "
			 "components\n",
                         _mesa_shader_stage_to_string(i));
         }
      }

      if (sh->num_combined_uniform_components >
	  ctx->Const.Program[i].MaxCombinedUniformComponents) {
         if (ctx->Const.GLSLSkipStrictMaxUniformLimitCheck) {
            linker_warning(prog, "Too many %s shader uniform components, "
                           "but the driver will try to optimize them out; "
                           "this is non-portable out-of-spec behavior\n",
                           _mesa_shader_stage_to_string(i));
         } else {
            linker_error(prog, "Too many %s shader uniform components\n",
                         _mesa_shader_stage_to_string(i));
         }
      }

      total_shader_storage_blocks += sh->NumShaderStorageBlocks;
      total_uniform_blocks += sh->NumUniformBlocks;

      const unsigned max_uniform_blocks =
         ctx->Const.Program[i].MaxUniformBlocks;
      if (max_uniform_blocks < sh->NumUniformBlocks) {
         linker_error(prog, "Too many %s uniform blocks (%d/%d)\n",
                      _mesa_shader_stage_to_string(i), sh->NumUniformBlocks,
                      max_uniform_blocks);
      }

      const unsigned max_shader_storage_blocks =
         ctx->Const.Program[i].MaxShaderStorageBlocks;
      if (max_shader_storage_blocks < sh->NumShaderStorageBlocks) {
         linker_error(prog, "Too many %s shader storage blocks (%d/%d)\n",
                      _mesa_shader_stage_to_string(i),
                      sh->NumShaderStorageBlocks, max_shader_storage_blocks);
      }
   }

   if (total_uniform_blocks > ctx->Const.MaxCombinedUniformBlocks) {
      linker_error(prog, "Too many combined uniform blocks (%d/%d)\n",
                   total_uniform_blocks, ctx->Const.MaxCombinedUniformBlocks);
   }

   if (total_shader_storage_blocks > ctx->Const.MaxCombinedShaderStorageBlocks) {
      linker_error(prog, "Too many combined shader storage blocks (%d/%d)\n",
                   total_shader_storage_blocks,
                   ctx->Const.MaxCombinedShaderStorageBlocks);
   }

   for (unsigned i = 0; i < prog->NumUniformBlocks; i++) {
      if (prog->UniformBlocks[i].UniformBufferSize >
          ctx->Const.MaxUniformBlockSize) {
         linker_error(prog, "Uniform block %s too big (%d/%d)\n",
                      prog->UniformBlocks[i].Name,
                      prog->UniformBlocks[i].UniformBufferSize,
                      ctx->Const.MaxUniformBlockSize);
      }
   }

   for (unsigned i = 0; i < prog->NumShaderStorageBlocks; i++) {
      if (prog->ShaderStorageBlocks[i].UniformBufferSize >
          ctx->Const.MaxShaderStorageBlockSize) {
         linker_error(prog, "Shader storage block %s too big (%d/%d)\n",
                      prog->ShaderStorageBlocks[i].Name,
                      prog->ShaderStorageBlocks[i].UniformBufferSize,
                      ctx->Const.MaxShaderStorageBlockSize);
      }
   }
}

static void
link_calculate_subroutine_compat(struct gl_shader_program *prog)
{
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      struct gl_shader *sh = prog->_LinkedShaders[i];
      int count;
      if (!sh)
         continue;

      for (unsigned j = 0; j < sh->NumSubroutineUniformRemapTable; j++) {
         if (sh->SubroutineUniformRemapTable[j] == INACTIVE_UNIFORM_EXPLICIT_LOCATION)
            continue;

         struct gl_uniform_storage *uni = sh->SubroutineUniformRemapTable[j];

         if (!uni)
            continue;

         sh->NumSubroutineUniforms++;
         count = 0;
         if (sh->NumSubroutineFunctions == 0) {
            linker_error(prog, "subroutine uniform %s defined but no valid functions found\n", uni->type->name);
            continue;
         }
         for (unsigned f = 0; f < sh->NumSubroutineFunctions; f++) {
            struct gl_subroutine_function *fn = &sh->SubroutineFunctions[f];
            for (int k = 0; k < fn->num_compat_types; k++) {
               if (fn->types[k] == uni->type) {
                  count++;
                  break;
               }
            }
         }
         uni->num_compatible_subroutines = count;
      }
   }
}

static void
check_subroutine_resources(struct gl_shader_program *prog)
{
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      struct gl_shader *sh = prog->_LinkedShaders[i];

      if (sh) {
         if (sh->NumSubroutineUniformRemapTable > MAX_SUBROUTINE_UNIFORM_LOCATIONS)
            linker_error(prog, "Too many %s shader subroutine uniforms\n",
                         _mesa_shader_stage_to_string(i));
      }
   }
}
/**
 * Validate shader image resources.
 */
static void
check_image_resources(struct gl_context *ctx, struct gl_shader_program *prog)
{
   unsigned total_image_units = 0;
   unsigned fragment_outputs = 0;
   unsigned total_shader_storage_blocks = 0;

   if (!ctx->Extensions.ARB_shader_image_load_store)
      return;

   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      struct gl_shader *sh = prog->_LinkedShaders[i];

      if (sh) {
         if (sh->NumImages > ctx->Const.Program[i].MaxImageUniforms)
            linker_error(prog, "Too many %s shader image uniforms (%u > %u)\n",
                         _mesa_shader_stage_to_string(i), sh->NumImages,
                         ctx->Const.Program[i].MaxImageUniforms);

         total_image_units += sh->NumImages;
         total_shader_storage_blocks += sh->NumShaderStorageBlocks;

         if (i == MESA_SHADER_FRAGMENT) {
            foreach_in_list(ir_instruction, node, sh->ir) {
               ir_variable *var = node->as_variable();
               if (var && var->data.mode == ir_var_shader_out)
                  /* since there are no double fs outputs - pass false */
                  fragment_outputs += var->type->count_attribute_slots(false);
            }
         }
      }
   }

   if (total_image_units > ctx->Const.MaxCombinedImageUniforms)
      linker_error(prog, "Too many combined image uniforms\n");

   if (total_image_units + fragment_outputs + total_shader_storage_blocks >
       ctx->Const.MaxCombinedShaderOutputResources)
      linker_error(prog, "Too many combined image uniforms, shader storage "
                         " buffers and fragment outputs\n");
}


/**
 * Initializes explicit location slots to INACTIVE_UNIFORM_EXPLICIT_LOCATION
 * for a variable, checks for overlaps between other uniforms using explicit
 * locations.
 */
static int
reserve_explicit_locations(struct gl_shader_program *prog,
                           string_to_uint_map *map, ir_variable *var)
{
   unsigned slots = var->type->uniform_locations();
   unsigned max_loc = var->data.location + slots - 1;
   unsigned return_value = slots;

   /* Resize remap table if locations do not fit in the current one. */
   if (max_loc + 1 > prog->NumUniformRemapTable) {
      prog->UniformRemapTable =
         reralloc(prog, prog->UniformRemapTable,
                  gl_uniform_storage *,
                  max_loc + 1);

      if (!prog->UniformRemapTable) {
         linker_error(prog, "Out of memory during linking.\n");
         return -1;
      }

      /* Initialize allocated space. */
      for (unsigned i = prog->NumUniformRemapTable; i < max_loc + 1; i++)
         prog->UniformRemapTable[i] = NULL;

      prog->NumUniformRemapTable = max_loc + 1;
   }

   for (unsigned i = 0; i < slots; i++) {
      unsigned loc = var->data.location + i;

      /* Check if location is already used. */
      if (prog->UniformRemapTable[loc] == INACTIVE_UNIFORM_EXPLICIT_LOCATION) {

         /* Possibly same uniform from a different stage, this is ok. */
         unsigned hash_loc;
         if (map->get(hash_loc, var->name) && hash_loc == loc - i) {
            return_value = 0;
            continue;
         }

         /* ARB_explicit_uniform_location specification states:
          *
          *     "No two default-block uniform variables in the program can have
          *     the same location, even if they are unused, otherwise a compiler
          *     or linker error will be generated."
          */
         linker_error(prog,
                      "location qualifier for uniform %s overlaps "
                      "previously used location\n",
                      var->name);
         return -1;
      }

      /* Initialize location as inactive before optimization
       * rounds and location assignment.
       */
      prog->UniformRemapTable[loc] = INACTIVE_UNIFORM_EXPLICIT_LOCATION;
   }

   /* Note, base location used for arrays. */
   map->put(var->data.location, var->name);

   return return_value;
}

static bool
reserve_subroutine_explicit_locations(struct gl_shader_program *prog,
                                      struct gl_shader *sh,
                                      ir_variable *var)
{
   unsigned slots = var->type->uniform_locations();
   unsigned max_loc = var->data.location + slots - 1;

   /* Resize remap table if locations do not fit in the current one. */
   if (max_loc + 1 > sh->NumSubroutineUniformRemapTable) {
      sh->SubroutineUniformRemapTable =
         reralloc(sh, sh->SubroutineUniformRemapTable,
                  gl_uniform_storage *,
                  max_loc + 1);

      if (!sh->SubroutineUniformRemapTable) {
         linker_error(prog, "Out of memory during linking.\n");
         return false;
      }

      /* Initialize allocated space. */
      for (unsigned i = sh->NumSubroutineUniformRemapTable; i < max_loc + 1; i++)
         sh->SubroutineUniformRemapTable[i] = NULL;

      sh->NumSubroutineUniformRemapTable = max_loc + 1;
   }

   for (unsigned i = 0; i < slots; i++) {
      unsigned loc = var->data.location + i;

      /* Check if location is already used. */
      if (sh->SubroutineUniformRemapTable[loc] == INACTIVE_UNIFORM_EXPLICIT_LOCATION) {

         /* ARB_explicit_uniform_location specification states:
          *     "No two subroutine uniform variables can have the same location
          *     in the same shader stage, otherwise a compiler or linker error
          *     will be generated."
          */
         linker_error(prog,
                      "location qualifier for uniform %s overlaps "
                      "previously used location\n",
                      var->name);
         return false;
      }

      /* Initialize location as inactive before optimization
       * rounds and location assignment.
       */
      sh->SubroutineUniformRemapTable[loc] = INACTIVE_UNIFORM_EXPLICIT_LOCATION;
   }

   return true;
}
/**
 * Check and reserve all explicit uniform locations, called before
 * any optimizations happen to handle also inactive uniforms and
 * inactive array elements that may get trimmed away.
 */
static unsigned
check_explicit_uniform_locations(struct gl_context *ctx,
                                 struct gl_shader_program *prog)
{
   if (!ctx->Extensions.ARB_explicit_uniform_location)
      return 0;

   /* This map is used to detect if overlapping explicit locations
    * occur with the same uniform (from different stage) or a different one.
    */
   string_to_uint_map *uniform_map = new string_to_uint_map;

   if (!uniform_map) {
      linker_error(prog, "Out of memory during linking.\n");
      return 0;
   }

   unsigned entries_total = 0;
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      struct gl_shader *sh = prog->_LinkedShaders[i];

      if (!sh)
         continue;

      foreach_in_list(ir_instruction, node, sh->ir) {
         ir_variable *var = node->as_variable();
         if (!var || var->data.mode != ir_var_uniform)
            continue;

         if (var->data.explicit_location) {
            bool ret = false;
            if (var->type->without_array()->is_subroutine())
               ret = reserve_subroutine_explicit_locations(prog, sh, var);
            else {
               int slots = reserve_explicit_locations(prog, uniform_map,
                                                      var);
               if (slots != -1) {
                  ret = true;
                  entries_total += slots;
               }
            }
            if (!ret) {
               delete uniform_map;
               return 0;
            }
         }
      }
   }

   struct empty_uniform_block *current_block = NULL;

   for (unsigned i = 0; i < prog->NumUniformRemapTable; i++) {
      /* We found empty space in UniformRemapTable. */
      if (prog->UniformRemapTable[i] == NULL) {
         /* We've found the beginning of a new continous block of empty slots */
         if (!current_block || current_block->start + current_block->slots != i) {
            current_block = rzalloc(prog, struct empty_uniform_block);
            current_block->start = i;
            exec_list_push_tail(&prog->EmptyUniformLocations,
                                &current_block->link);
         }

         /* The current block continues, so we simply increment its slots */
         current_block->slots++;
      }
   }

   delete uniform_map;
   return entries_total;
}

static bool
should_add_buffer_variable(struct gl_shader_program *shProg,
                           GLenum type, const char *name)
{
   bool found_interface = false;
   unsigned block_name_len = 0;
   const char *block_name_dot = strchr(name, '.');

   /* These rules only apply to buffer variables. So we return
    * true for the rest of types.
    */
   if (type != GL_BUFFER_VARIABLE)
      return true;

   for (unsigned i = 0; i < shProg->NumShaderStorageBlocks; i++) {
      const char *block_name = shProg->ShaderStorageBlocks[i].Name;
      block_name_len = strlen(block_name);

      const char *block_square_bracket = strchr(block_name, '[');
      if (block_square_bracket) {
         /* The block is part of an array of named interfaces,
          * for the name comparison we ignore the "[x]" part.
          */
         block_name_len -= strlen(block_square_bracket);
      }

      if (block_name_dot) {
         /* Check if the variable name starts with the interface
          * name. The interface name (if present) should have the
          * length than the interface block name we are comparing to.
          */
         unsigned len = strlen(name) - strlen(block_name_dot);
         if (len != block_name_len)
            continue;
      }

      if (strncmp(block_name, name, block_name_len) == 0) {
         found_interface = true;
         break;
      }
   }

   /* We remove the interface name from the buffer variable name,
    * including the dot that follows it.
    */
   if (found_interface)
      name = name + block_name_len + 1;

   /* The ARB_program_interface_query spec says:
    *
    *     "For an active shader storage block member declared as an array, an
    *     entry will be generated only for the first array element, regardless
    *     of its type.  For arrays of aggregate types, the enumeration rules
    *     are applied recursively for the single enumerated array element."
    */
   const char *struct_first_dot = strchr(name, '.');
   const char *first_square_bracket = strchr(name, '[');

   /* The buffer variable is on top level and it is not an array */
   if (!first_square_bracket) {
      return true;
   /* The shader storage block member is a struct, then generate the entry */
   } else if (struct_first_dot && struct_first_dot < first_square_bracket) {
      return true;
   } else {
      /* Shader storage block member is an array, only generate an entry for the
       * first array element.
       */
      if (strncmp(first_square_bracket, "[0]", 3) == 0)
         return true;
   }

   return false;
}

static bool
add_program_resource(struct gl_shader_program *prog, GLenum type,
                     const void *data, uint8_t stages)
{
   assert(data);

   /* If resource already exists, do not add it again. */
   for (unsigned i = 0; i < prog->NumProgramResourceList; i++)
      if (prog->ProgramResourceList[i].Data == data)
         return true;

   prog->ProgramResourceList =
      reralloc(prog,
               prog->ProgramResourceList,
               gl_program_resource,
               prog->NumProgramResourceList + 1);

   if (!prog->ProgramResourceList) {
      linker_error(prog, "Out of memory during linking.\n");
      return false;
   }

   struct gl_program_resource *res =
      &prog->ProgramResourceList[prog->NumProgramResourceList];

   res->Type = type;
   res->Data = data;
   res->StageReferences = stages;

   prog->NumProgramResourceList++;

   return true;
}

/* Function checks if a variable var is a packed varying and
 * if given name is part of packed varying's list.
 *
 * If a variable is a packed varying, it has a name like
 * 'packed:a,b,c' where a, b and c are separate variables.
 */
static bool
included_in_packed_varying(ir_variable *var, const char *name)
{
   if (strncmp(var->name, "packed:", 7) != 0)
      return false;

   char *list = strdup(var->name + 7);
   assert(list);

   bool found = false;
   char *saveptr;
   char *token = strtok_r(list, ",", &saveptr);
   while (token) {
      if (strcmp(token, name) == 0) {
         found = true;
         break;
      }
      token = strtok_r(NULL, ",", &saveptr);
   }
   free(list);
   return found;
}

/**
 * Function builds a stage reference bitmask from variable name.
 */
static uint8_t
build_stageref(struct gl_shader_program *shProg, const char *name,
               unsigned mode)
{
   uint8_t stages = 0;

   /* Note, that we assume MAX 8 stages, if there will be more stages, type
    * used for reference mask in gl_program_resource will need to be changed.
    */
   assert(MESA_SHADER_STAGES < 8);

   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      struct gl_shader *sh = shProg->_LinkedShaders[i];
      if (!sh)
         continue;

      /* Shader symbol table may contain variables that have
       * been optimized away. Search IR for the variable instead.
       */
      foreach_in_list(ir_instruction, node, sh->ir) {
         ir_variable *var = node->as_variable();
         if (var) {
            unsigned baselen = strlen(var->name);

            if (included_in_packed_varying(var, name)) {
                  stages |= (1 << i);
                  break;
            }

            /* Type needs to match if specified, otherwise we might
             * pick a variable with same name but different interface.
             */
            if (var->data.mode != mode)
               continue;

            if (strncmp(var->name, name, baselen) == 0) {
               /* Check for exact name matches but also check for arrays and
                * structs.
                */
               if (name[baselen] == '\0' ||
                   name[baselen] == '[' ||
                   name[baselen] == '.') {
                  stages |= (1 << i);
                  break;
               }
            }
         }
      }
   }
   return stages;
}

/**
 * Create gl_shader_variable from ir_variable class.
 */
static gl_shader_variable *
create_shader_variable(struct gl_shader_program *shProg,
                       const ir_variable *in,
                       const char *name, const glsl_type *type,
                       bool use_implicit_location, int location,
                       const glsl_type *outermost_struct_type)
{
   gl_shader_variable *out = ralloc(shProg, struct gl_shader_variable);
   if (!out)
      return NULL;

   /* Since gl_VertexID may be lowered to gl_VertexIDMESA, but applications
    * expect to see gl_VertexID in the program resource list.  Pretend.
    */
   if (in->data.mode == ir_var_system_value &&
       in->data.location == SYSTEM_VALUE_VERTEX_ID_ZERO_BASE) {
      out->name = ralloc_strdup(shProg, "gl_VertexID");
   } else {
      out->name = ralloc_strdup(shProg, name);
   }

   if (!out->name)
      return NULL;

   /* The ARB_program_interface_query spec says:
    *
    *     "Not all active variables are assigned valid locations; the
    *     following variables will have an effective location of -1:
    *
    *      * uniforms declared as atomic counters;
    *
    *      * members of a uniform block;
    *
    *      * built-in inputs, outputs, and uniforms (starting with "gl_"); and
    *
    *      * inputs or outputs not declared with a "location" layout
    *        qualifier, except for vertex shader inputs and fragment shader
    *        outputs."
    */
   if (in->type->base_type == GLSL_TYPE_ATOMIC_UINT ||
       is_gl_identifier(in->name) ||
       !(in->data.explicit_location || use_implicit_location)) {
      out->location = -1;
   } else {
      out->location = location;
   }

   out->type = type;
   out->outermost_struct_type = outermost_struct_type;
   out->interface_type = in->get_interface_type();
   out->component = in->data.location_frac;
   out->index = in->data.index;
   out->patch = in->data.patch;
   out->mode = in->data.mode;
   out->interpolation = in->data.interpolation;
   out->explicit_location = in->data.explicit_location;
   out->precision = in->data.precision;

   return out;
}

static bool
add_shader_variable(struct gl_shader_program *shProg, unsigned stage_mask,
                    GLenum programInterface, ir_variable *var,
                    const char *name, const glsl_type *type,
                    bool use_implicit_location, int location,
                    const glsl_type *outermost_struct_type = NULL)
{
   const bool is_vertex_input =
      programInterface == GL_PROGRAM_INPUT &&
      stage_mask == MESA_SHADER_VERTEX;

   switch (type->base_type) {
   case GLSL_TYPE_STRUCT: {
      /* The ARB_program_interface_query spec says:
       *
       *     "For an active variable declared as a structure, a separate entry
       *     will be generated for each active structure member.  The name of
       *     each entry is formed by concatenating the name of the structure,
       *     the "."  character, and the name of the structure member.  If a
       *     structure member to enumerate is itself a structure or array,
       *     these enumeration rules are applied recursively."
       */
      if (outermost_struct_type == NULL)
         outermost_struct_type = type;

      unsigned field_location = location;
      for (unsigned i = 0; i < type->length; i++) {
         const struct glsl_struct_field *field = &type->fields.structure[i];
         char *field_name = ralloc_asprintf(shProg, "%s.%s", name, field->name);
         if (!add_shader_variable(shProg, stage_mask, programInterface,
                                  var, field_name, field->type,
                                  use_implicit_location, field_location,
                                  outermost_struct_type))
            return false;

         field_location +=
            field->type->count_attribute_slots(is_vertex_input);
      }
      return true;
   }

   default: {
      /* Issue #16 of the ARB_program_interface_query spec says:
       *
       * "* If a variable is a member of an interface block without an
       *    instance name, it is enumerated using just the variable name.
       *
       *  * If a variable is a member of an interface block with an instance
       *    name, it is enumerated as "BlockName.Member", where "BlockName" is
       *    the name of the interface block (not the instance name) and
       *    "Member" is the name of the variable."
       */
      const char *prefixed_name = (var->data.from_named_ifc_block &&
                                   !is_gl_identifier(var->name))
         ? ralloc_asprintf(shProg, "%s.%s", var->get_interface_type()->name,
                           name)
         : name;

      /* The ARB_program_interface_query spec says:
       *
       *     "For an active variable declared as a single instance of a basic
       *     type, a single entry will be generated, using the variable name
       *     from the shader source."
       */
      gl_shader_variable *sha_v =
         create_shader_variable(shProg, var, prefixed_name, type,
                                use_implicit_location, location,
                                outermost_struct_type);
      if (!sha_v)
         return false;

      return add_program_resource(shProg, programInterface, sha_v, stage_mask);
   }
   }
}

static bool
add_interface_variables(struct gl_shader_program *shProg,
                        unsigned stage, GLenum programInterface)
{
   exec_list *ir = shProg->_LinkedShaders[stage]->ir;

   foreach_in_list(ir_instruction, node, ir) {
      ir_variable *var = node->as_variable();

      if (!var || var->data.how_declared == ir_var_hidden)
         continue;

      int loc_bias;

      switch (var->data.mode) {
      case ir_var_system_value:
      case ir_var_shader_in:
         if (programInterface != GL_PROGRAM_INPUT)
            continue;
         loc_bias = (stage == MESA_SHADER_VERTEX) ? int(VERT_ATTRIB_GENERIC0)
                                                  : int(VARYING_SLOT_VAR0);
         break;
      case ir_var_shader_out:
         if (programInterface != GL_PROGRAM_OUTPUT)
            continue;
         loc_bias = (stage == MESA_SHADER_FRAGMENT) ? int(FRAG_RESULT_DATA0)
                                                    : int(VARYING_SLOT_VAR0);
         break;
      default:
         continue;
      };

      /* Skip packed varyings, packed varyings are handled separately
       * by add_packed_varyings.
       */
      if (strncmp(var->name, "packed:", 7) == 0)
         continue;

      /* Skip fragdata arrays, these are handled separately
       * by add_fragdata_arrays.
       */
      if (strncmp(var->name, "gl_out_FragData", 15) == 0)
         continue;

      const bool vs_input_or_fs_output =
         (stage == MESA_SHADER_VERTEX && var->data.mode == ir_var_shader_in) ||
         (stage == MESA_SHADER_FRAGMENT && var->data.mode == ir_var_shader_out);

      if (!add_shader_variable(shProg, 1 << stage, programInterface,
                               var, var->name, var->type, vs_input_or_fs_output,
                               var->data.location - loc_bias))
         return false;
   }
   return true;
}

static bool
add_packed_varyings(struct gl_shader_program *shProg, int stage, GLenum type)
{
   struct gl_shader *sh = shProg->_LinkedShaders[stage];
   GLenum iface;

   if (!sh || !sh->packed_varyings)
      return true;

   foreach_in_list(ir_instruction, node, sh->packed_varyings) {
      ir_variable *var = node->as_variable();
      if (var) {
         switch (var->data.mode) {
         case ir_var_shader_in:
            iface = GL_PROGRAM_INPUT;
            break;
         case ir_var_shader_out:
            iface = GL_PROGRAM_OUTPUT;
            break;
         default:
            unreachable("unexpected type");
         }

         if (type == iface) {
            const int stage_mask =
               build_stageref(shProg, var->name, var->data.mode);
            if (!add_shader_variable(shProg, stage_mask,
                                     iface, var, var->name, var->type, false,
                                     var->data.location - VARYING_SLOT_VAR0))
               return false;
         }
      }
   }
   return true;
}

static bool
add_fragdata_arrays(struct gl_shader_program *shProg)
{
   struct gl_shader *sh = shProg->_LinkedShaders[MESA_SHADER_FRAGMENT];

   if (!sh || !sh->fragdata_arrays)
      return true;

   foreach_in_list(ir_instruction, node, sh->fragdata_arrays) {
      ir_variable *var = node->as_variable();
      if (var) {
         assert(var->data.mode == ir_var_shader_out);

         if (!add_shader_variable(shProg,
                                  1 << MESA_SHADER_FRAGMENT,
                                  GL_PROGRAM_OUTPUT, var, var->name, var->type,
                                  true, var->data.location - FRAG_RESULT_DATA0))
            return false;
      }
   }
   return true;
}

static char*
get_top_level_name(const char *name)
{
   const char *first_dot = strchr(name, '.');
   const char *first_square_bracket = strchr(name, '[');
   int name_size = 0;

   /* The ARB_program_interface_query spec says:
    *
    *     "For the property TOP_LEVEL_ARRAY_SIZE, a single integer identifying
    *     the number of active array elements of the top-level shader storage
    *     block member containing to the active variable is written to
    *     <params>.  If the top-level block member is not declared as an
    *     array, the value one is written to <params>.  If the top-level block
    *     member is an array with no declared size, the value zero is written
    *     to <params>."
    */

   /* The buffer variable is on top level.*/
   if (!first_square_bracket && !first_dot)
      name_size = strlen(name);
   else if ((!first_square_bracket ||
            (first_dot && first_dot < first_square_bracket)))
      name_size = first_dot - name;
   else
      name_size = first_square_bracket - name;

   return strndup(name, name_size);
}

static char*
get_var_name(const char *name)
{
   const char *first_dot = strchr(name, '.');

   if (!first_dot)
      return strdup(name);

   return strndup(first_dot+1, strlen(first_dot) - 1);
}

static bool
is_top_level_shader_storage_block_member(const char* name,
                                         const char* interface_name,
                                         const char* field_name)
{
   bool result = false;

   /* If the given variable is already a top-level shader storage
    * block member, then return array_size = 1.
    * We could have two possibilities: if we have an instanced
    * shader storage block or not instanced.
    *
    * For the first, we check create a name as it was in top level and
    * compare it with the real name. If they are the same, then
    * the variable is already at top-level.
    *
    * Full instanced name is: interface name + '.' + var name +
    *    NULL character
    */
   int name_length = strlen(interface_name) + 1 + strlen(field_name) + 1;
   char *full_instanced_name = (char *) calloc(name_length, sizeof(char));
   if (!full_instanced_name) {
      fprintf(stderr, "%s: Cannot allocate space for name\n", __func__);
      return false;
   }

   snprintf(full_instanced_name, name_length, "%s.%s",
            interface_name, field_name);

   /* Check if its top-level shader storage block member of an
    * instanced interface block, or of a unnamed interface block.
    */
   if (strcmp(name, full_instanced_name) == 0 ||
       strcmp(name, field_name) == 0)
      result = true;

   free(full_instanced_name);
   return result;
}

static int
get_array_size(struct gl_uniform_storage *uni, const glsl_struct_field *field,
               char *interface_name, char *var_name)
{
   /* The ARB_program_interface_query spec says:
    *
    *     "For the property TOP_LEVEL_ARRAY_SIZE, a single integer identifying
    *     the number of active array elements of the top-level shader storage
    *     block member containing to the active variable is written to
    *     <params>.  If the top-level block member is not declared as an
    *     array, the value one is written to <params>.  If the top-level block
    *     member is an array with no declared size, the value zero is written
    *     to <params>."
    */
   if (is_top_level_shader_storage_block_member(uni->name,
                                                interface_name,
                                                var_name))
      return  1;
   else if (field->type->is_unsized_array())
      return 0;
   else if (field->type->is_array())
      return field->type->length;

   return 1;
}

static int
get_array_stride(struct gl_uniform_storage *uni, const glsl_type *interface,
                 const glsl_struct_field *field, char *interface_name,
                 char *var_name)
{
   /* The ARB_program_interface_query spec says:
    *
    *     "For the property TOP_LEVEL_ARRAY_STRIDE, a single integer
    *     identifying the stride between array elements of the top-level
    *     shader storage block member containing the active variable is
    *     written to <params>.  For top-level block members declared as
    *     arrays, the value written is the difference, in basic machine units,
    *     between the offsets of the active variable for consecutive elements
    *     in the top-level array.  For top-level block members not declared as
    *     an array, zero is written to <params>."
    */
   if (field->type->is_array()) {
      const enum glsl_matrix_layout matrix_layout =
         glsl_matrix_layout(field->matrix_layout);
      bool row_major = matrix_layout == GLSL_MATRIX_LAYOUT_ROW_MAJOR;
      const glsl_type *array_type = field->type->fields.array;

      if (is_top_level_shader_storage_block_member(uni->name,
                                                   interface_name,
                                                   var_name))
         return 0;

      if (interface->interface_packing != GLSL_INTERFACE_PACKING_STD430) {
         if (array_type->is_record() || array_type->is_array())
            return glsl_align(array_type->std140_size(row_major), 16);
         else
            return MAX2(array_type->std140_base_alignment(row_major), 16);
      } else {
         return array_type->std430_array_stride(row_major);
      }
   }
   return 0;
}

static void
calculate_array_size_and_stride(struct gl_shader_program *shProg,
                                struct gl_uniform_storage *uni)
{
   int block_index = uni->block_index;
   int array_size = -1;
   int array_stride = -1;
   char *var_name = get_top_level_name(uni->name);
   char *interface_name =
      get_top_level_name(uni->is_shader_storage ?
                         shProg->ShaderStorageBlocks[block_index].Name :
                         shProg->UniformBlocks[block_index].Name);

   if (strcmp(var_name, interface_name) == 0) {
      /* Deal with instanced array of SSBOs */
      char *temp_name = get_var_name(uni->name);
      if (!temp_name) {
         linker_error(shProg, "Out of memory during linking.\n");
         goto write_top_level_array_size_and_stride;
      }
      free(var_name);
      var_name = get_top_level_name(temp_name);
      free(temp_name);
      if (!var_name) {
         linker_error(shProg, "Out of memory during linking.\n");
         goto write_top_level_array_size_and_stride;
      }
   }

   for (unsigned i = 0; i < shProg->NumShaders; i++) {
      if (shProg->Shaders[i] == NULL)
         continue;

      const gl_shader *stage = shProg->Shaders[i];
      foreach_in_list(ir_instruction, node, stage->ir) {
         ir_variable *var = node->as_variable();
         if (!var || !var->get_interface_type() ||
             var->data.mode != ir_var_shader_storage)
            continue;

         const glsl_type *interface = var->get_interface_type();

         if (strcmp(interface_name, interface->name) != 0)
            continue;

         for (unsigned i = 0; i < interface->length; i++) {
            const glsl_struct_field *field = &interface->fields.structure[i];
            if (strcmp(field->name, var_name) != 0)
               continue;

            array_stride = get_array_stride(uni, interface, field,
                                            interface_name, var_name);
            array_size = get_array_size(uni, field, interface_name, var_name);
            goto write_top_level_array_size_and_stride;
         }
      }
   }
write_top_level_array_size_and_stride:
   free(interface_name);
   free(var_name);
   uni->top_level_array_stride = array_stride;
   uni->top_level_array_size = array_size;
}

/**
 * Builds up a list of program resources that point to existing
 * resource data.
 */
void
build_program_resource_list(struct gl_context *ctx,
                            struct gl_shader_program *shProg)
{
   /* Rebuild resource list. */
   if (shProg->ProgramResourceList) {
      ralloc_free(shProg->ProgramResourceList);
      shProg->ProgramResourceList = NULL;
      shProg->NumProgramResourceList = 0;
   }

   int input_stage = MESA_SHADER_STAGES, output_stage = 0;

   /* Determine first input and final output stage. These are used to
    * detect which variables should be enumerated in the resource list
    * for GL_PROGRAM_INPUT and GL_PROGRAM_OUTPUT.
    */
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      if (!shProg->_LinkedShaders[i])
         continue;
      if (input_stage == MESA_SHADER_STAGES)
         input_stage = i;
      output_stage = i;
   }

   /* Empty shader, no resources. */
   if (input_stage == MESA_SHADER_STAGES && output_stage == 0)
      return;

   /* Program interface needs to expose varyings in case of SSO. */
   if (shProg->SeparateShader) {
      if (!add_packed_varyings(shProg, input_stage, GL_PROGRAM_INPUT))
         return;

      if (!add_packed_varyings(shProg, output_stage, GL_PROGRAM_OUTPUT))
         return;
   }

   if (!add_fragdata_arrays(shProg))
      return;

   /* Add inputs and outputs to the resource list. */
   if (!add_interface_variables(shProg, input_stage, GL_PROGRAM_INPUT))
      return;

   if (!add_interface_variables(shProg, output_stage, GL_PROGRAM_OUTPUT))
      return;

   /* Add transform feedback varyings. */
   if (shProg->LinkedTransformFeedback.NumVarying > 0) {
      for (int i = 0; i < shProg->LinkedTransformFeedback.NumVarying; i++) {
         if (!add_program_resource(shProg, GL_TRANSFORM_FEEDBACK_VARYING,
                                   &shProg->LinkedTransformFeedback.Varyings[i],
                                   0))
         return;
      }
   }

   /* Add transform feedback buffers. */
   for (unsigned i = 0; i < ctx->Const.MaxTransformFeedbackBuffers; i++) {
      if ((shProg->LinkedTransformFeedback.ActiveBuffers >> i) & 1) {
         shProg->LinkedTransformFeedback.Buffers[i].Binding = i;
         if (!add_program_resource(shProg, GL_TRANSFORM_FEEDBACK_BUFFER,
                                   &shProg->LinkedTransformFeedback.Buffers[i],
                                   0))
         return;
      }
   }

   /* Add uniforms from uniform storage. */
   for (unsigned i = 0; i < shProg->NumUniformStorage; i++) {
      /* Do not add uniforms internally used by Mesa. */
      if (shProg->UniformStorage[i].hidden)
         continue;

      uint8_t stageref =
         build_stageref(shProg, shProg->UniformStorage[i].name,
                        ir_var_uniform);

      /* Add stagereferences for uniforms in a uniform block. */
      bool is_shader_storage =  shProg->UniformStorage[i].is_shader_storage;
      int block_index = shProg->UniformStorage[i].block_index;
      if (block_index != -1) {
         stageref |= is_shader_storage ?
            shProg->ShaderStorageBlocks[block_index].stageref :
            shProg->UniformBlocks[block_index].stageref;
      }

      GLenum type = is_shader_storage ? GL_BUFFER_VARIABLE : GL_UNIFORM;
      if (!should_add_buffer_variable(shProg, type,
                                      shProg->UniformStorage[i].name))
         continue;

      if (is_shader_storage) {
         calculate_array_size_and_stride(shProg, &shProg->UniformStorage[i]);
      }

      if (!add_program_resource(shProg, type,
                                &shProg->UniformStorage[i], stageref))
         return;
   }

   /* Add program uniform blocks. */
   for (unsigned i = 0; i < shProg->NumUniformBlocks; i++) {
      if (!add_program_resource(shProg, GL_UNIFORM_BLOCK,
          &shProg->UniformBlocks[i], 0))
         return;
   }

   /* Add program shader storage blocks. */
   for (unsigned i = 0; i < shProg->NumShaderStorageBlocks; i++) {
      if (!add_program_resource(shProg, GL_SHADER_STORAGE_BLOCK,
          &shProg->ShaderStorageBlocks[i], 0))
         return;
   }

   /* Add atomic counter buffers. */
   for (unsigned i = 0; i < shProg->NumAtomicBuffers; i++) {
      if (!add_program_resource(shProg, GL_ATOMIC_COUNTER_BUFFER,
                                &shProg->AtomicBuffers[i], 0))
         return;
   }

   for (unsigned i = 0; i < shProg->NumUniformStorage; i++) {
      GLenum type;
      if (!shProg->UniformStorage[i].hidden)
         continue;

      for (int j = MESA_SHADER_VERTEX; j < MESA_SHADER_STAGES; j++) {
         if (!shProg->UniformStorage[i].opaque[j].active ||
             !shProg->UniformStorage[i].type->is_subroutine())
            continue;

         type = _mesa_shader_stage_to_subroutine_uniform((gl_shader_stage)j);
         /* add shader subroutines */
         if (!add_program_resource(shProg, type, &shProg->UniformStorage[i], 0))
            return;
      }
   }

   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      struct gl_shader *sh = shProg->_LinkedShaders[i];
      GLuint type;

      if (!sh)
         continue;

      type = _mesa_shader_stage_to_subroutine((gl_shader_stage)i);
      for (unsigned j = 0; j < sh->NumSubroutineFunctions; j++) {
         if (!add_program_resource(shProg, type, &sh->SubroutineFunctions[j], 0))
            return;
      }
   }
}

/**
 * This check is done to make sure we allow only constant expression
 * indexing and "constant-index-expression" (indexing with an expression
 * that includes loop induction variable).
 */
static bool
validate_sampler_array_indexing(struct gl_context *ctx,
                                struct gl_shader_program *prog)
{
   dynamic_sampler_array_indexing_visitor v;
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      if (prog->_LinkedShaders[i] == NULL)
	 continue;

      bool no_dynamic_indexing =
         ctx->Const.ShaderCompilerOptions[i].EmitNoIndirectSampler;

      /* Search for array derefs in shader. */
      v.run(prog->_LinkedShaders[i]->ir);
      if (v.uses_dynamic_sampler_array_indexing()) {
         const char *msg = "sampler arrays indexed with non-constant "
                           "expressions is forbidden in GLSL %s %u";
         /* Backend has indicated that it has no dynamic indexing support. */
         if (no_dynamic_indexing) {
            linker_error(prog, msg, prog->IsES ? "ES" : "", prog->Version);
            return false;
         } else {
            linker_warning(prog, msg, prog->IsES ? "ES" : "", prog->Version);
         }
      }
   }
   return true;
}

static void
link_assign_subroutine_types(struct gl_shader_program *prog)
{
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      gl_shader *sh = prog->_LinkedShaders[i];

      if (sh == NULL)
         continue;

      sh->MaxSubroutineFunctionIndex = 0;
      foreach_in_list(ir_instruction, node, sh->ir) {
         ir_function *fn = node->as_function();
         if (!fn)
            continue;

         if (fn->is_subroutine)
            sh->NumSubroutineUniformTypes++;

         if (!fn->num_subroutine_types)
            continue;

	 /* these should have been calculated earlier. */
	 assert(fn->subroutine_index != -1);
         if (sh->NumSubroutineFunctions + 1 > MAX_SUBROUTINES) {
            linker_error(prog, "Too many subroutine functions declared.\n");
            return;
         }
         sh->SubroutineFunctions = reralloc(sh, sh->SubroutineFunctions,
                                            struct gl_subroutine_function,
                                            sh->NumSubroutineFunctions + 1);
         sh->SubroutineFunctions[sh->NumSubroutineFunctions].name = ralloc_strdup(sh, fn->name);
         sh->SubroutineFunctions[sh->NumSubroutineFunctions].num_compat_types = fn->num_subroutine_types;
         sh->SubroutineFunctions[sh->NumSubroutineFunctions].types =
            ralloc_array(sh, const struct glsl_type *,
                         fn->num_subroutine_types);

         /* From Section 4.4.4(Subroutine Function Layout Qualifiers) of the
          * GLSL 4.5 spec:
          *
          *    "Each subroutine with an index qualifier in the shader must be
          *    given a unique index, otherwise a compile or link error will be
          *    generated."
          */
         for (unsigned j = 0; j < sh->NumSubroutineFunctions; j++) {
            if (sh->SubroutineFunctions[j].index != -1 &&
                sh->SubroutineFunctions[j].index == fn->subroutine_index) {
               linker_error(prog, "each subroutine index qualifier in the "
                            "shader must be unique\n");
               return;
            }
         }
         sh->SubroutineFunctions[sh->NumSubroutineFunctions].index =
            fn->subroutine_index;

         if (fn->subroutine_index > (int)sh->MaxSubroutineFunctionIndex)
            sh->MaxSubroutineFunctionIndex = fn->subroutine_index;

         for (int j = 0; j < fn->num_subroutine_types; j++)
            sh->SubroutineFunctions[sh->NumSubroutineFunctions].types[j] = fn->subroutine_types[j];
         sh->NumSubroutineFunctions++;
      }
   }
}

static void
set_always_active_io(exec_list *ir, ir_variable_mode io_mode)
{
   assert(io_mode == ir_var_shader_in || io_mode == ir_var_shader_out);

   foreach_in_list(ir_instruction, node, ir) {
      ir_variable *const var = node->as_variable();

      if (var == NULL || var->data.mode != io_mode)
         continue;

      /* Don't set always active on builtins that haven't been redeclared */
      if (var->data.how_declared == ir_var_declared_implicitly)
         continue;

      var->data.always_active_io = true;
   }
}

/**
 * When separate shader programs are enabled, only input/outputs between
 * the stages of a multi-stage separate program can be safely removed
 * from the shader interface. Other inputs/outputs must remain active.
 */
static void
disable_varying_optimizations_for_sso(struct gl_shader_program *prog)
{
   unsigned first, last;
   assert(prog->SeparateShader);

   first = MESA_SHADER_STAGES;
   last = 0;

   /* Determine first and last stage. Excluding the compute stage */
   for (unsigned i = 0; i < MESA_SHADER_COMPUTE; i++) {
      if (!prog->_LinkedShaders[i])
         continue;
      if (first == MESA_SHADER_STAGES)
         first = i;
      last = i;
   }

   if (first == MESA_SHADER_STAGES)
      return;

   for (unsigned stage = 0; stage < MESA_SHADER_STAGES; stage++) {
      gl_shader *sh = prog->_LinkedShaders[stage];
      if (!sh)
         continue;

      if (first == last) {
         /* For a single shader program only allow inputs to the vertex shader
          * and outputs from the fragment shader to be removed.
          */
         if (stage != MESA_SHADER_VERTEX)
            set_always_active_io(sh->ir, ir_var_shader_in);
         if (stage != MESA_SHADER_FRAGMENT)
            set_always_active_io(sh->ir, ir_var_shader_out);
      } else {
         /* For multi-stage separate shader programs only allow inputs and
          * outputs between the shader stages to be removed as well as inputs
          * to the vertex shader and outputs from the fragment shader.
          */
         if (stage == first && stage != MESA_SHADER_VERTEX)
            set_always_active_io(sh->ir, ir_var_shader_in);
         else if (stage == last && stage != MESA_SHADER_FRAGMENT)
            set_always_active_io(sh->ir, ir_var_shader_out);
      }
   }
}

void
link_shaders(struct gl_context *ctx, struct gl_shader_program *prog)
{
   prog->LinkStatus = true; /* All error paths will set this to false */
   prog->Validated = false;
   prog->_Used = false;

   /* Section 7.3 (Program Objects) of the OpenGL 4.5 Core Profile spec says:
    *
    *     "Linking can fail for a variety of reasons as specified in the
    *     OpenGL Shading Language Specification, as well as any of the
    *     following reasons:
    *
    *     - No shader objects are attached to program."
    *
    * The Compatibility Profile specification does not list the error.  In
    * Compatibility Profile missing shader stages are replaced by
    * fixed-function.  This applies to the case where all stages are
    * missing.
    */
   if (prog->NumShaders == 0) {
      if (ctx->API != API_OPENGL_COMPAT)
         linker_error(prog, "no shaders attached to the program\n");
      return;
   }

   unsigned num_tfeedback_decls = 0;
   unsigned int num_explicit_uniform_locs = 0;
   bool has_xfb_qualifiers = false;
   char **varying_names = NULL;
   tfeedback_decl *tfeedback_decls = NULL;

   void *mem_ctx = ralloc_context(NULL); // temporary linker context

   prog->ARB_fragment_coord_conventions_enable = false;

   /* Separate the shaders into groups based on their type.
    */
   struct gl_shader **shader_list[MESA_SHADER_STAGES];
   unsigned num_shaders[MESA_SHADER_STAGES];

   for (int i = 0; i < MESA_SHADER_STAGES; i++) {
      shader_list[i] = (struct gl_shader **)
         calloc(prog->NumShaders, sizeof(struct gl_shader *));
      num_shaders[i] = 0;
   }

   unsigned min_version = UINT_MAX;
   unsigned max_version = 0;
   for (unsigned i = 0; i < prog->NumShaders; i++) {
      min_version = MIN2(min_version, prog->Shaders[i]->Version);
      max_version = MAX2(max_version, prog->Shaders[i]->Version);

      if (prog->Shaders[i]->IsES != prog->Shaders[0]->IsES) {
	 linker_error(prog, "all shaders must use same shading "
		      "language version\n");
	 goto done;
      }

      if (prog->Shaders[i]->ARB_fragment_coord_conventions_enable) {
         prog->ARB_fragment_coord_conventions_enable = true;
      }

      gl_shader_stage shader_type = prog->Shaders[i]->Stage;
      shader_list[shader_type][num_shaders[shader_type]] = prog->Shaders[i];
      num_shaders[shader_type]++;
   }

   /* In desktop GLSL, different shader versions may be linked together.  In
    * GLSL ES, all shader versions must be the same.
    */
   if (prog->Shaders[0]->IsES && min_version != max_version) {
      linker_error(prog, "all shaders must use same shading "
		   "language version\n");
      goto done;
   }

   prog->Version = max_version;
   prog->IsES = prog->Shaders[0]->IsES;

   /* Some shaders have to be linked with some other shaders present.
    */
   if (!prog->SeparateShader) {
      if (num_shaders[MESA_SHADER_GEOMETRY] > 0 &&
          num_shaders[MESA_SHADER_VERTEX] == 0) {
         linker_error(prog, "Geometry shader must be linked with "
		      "vertex shader\n");
         goto done;
      }
      if (num_shaders[MESA_SHADER_TESS_EVAL] > 0 &&
          num_shaders[MESA_SHADER_VERTEX] == 0) {
         linker_error(prog, "Tessellation evaluation shader must be linked "
		      "with vertex shader\n");
         goto done;
      }
      if (num_shaders[MESA_SHADER_TESS_CTRL] > 0 &&
          num_shaders[MESA_SHADER_VERTEX] == 0) {
         linker_error(prog, "Tessellation control shader must be linked with "
		      "vertex shader\n");
         goto done;
      }

      /* The spec is self-contradictory here. It allows linking without a tess
       * eval shader, but that can only be used with transform feedback and
       * rasterization disabled. However, transform feedback isn't allowed
       * with GL_PATCHES, so it can't be used.
       *
       * More investigation showed that the idea of transform feedback after
       * a tess control shader was dropped, because some hw vendors couldn't
       * support tessellation without a tess eval shader, but the linker
       * section wasn't updated to reflect that.
       *
       * All specifications (ARB_tessellation_shader, GL 4.0-4.5) have this
       * spec bug.
       *
       * Do what's reasonable and always require a tess eval shader if a tess
       * control shader is present.
       */
      if (num_shaders[MESA_SHADER_TESS_CTRL] > 0 &&
          num_shaders[MESA_SHADER_TESS_EVAL] == 0) {
         linker_error(prog, "Tessellation control shader must be linked with "
		      "tessellation evaluation shader\n");
         goto done;
      }
   }

   /* Compute shaders have additional restrictions. */
   if (num_shaders[MESA_SHADER_COMPUTE] > 0 &&
       num_shaders[MESA_SHADER_COMPUTE] != prog->NumShaders) {
      linker_error(prog, "Compute shaders may not be linked with any other "
                   "type of shader\n");
   }

   for (unsigned int i = 0; i < MESA_SHADER_STAGES; i++) {
      if (prog->_LinkedShaders[i] != NULL)
	 _mesa_delete_shader(ctx, prog->_LinkedShaders[i]);

      prog->_LinkedShaders[i] = NULL;
   }

   /* Link all shaders for a particular stage and validate the result.
    */
   for (int stage = 0; stage < MESA_SHADER_STAGES; stage++) {
      if (num_shaders[stage] > 0) {
         gl_shader *const sh =
            link_intrastage_shaders(mem_ctx, ctx, prog, shader_list[stage],
                                    num_shaders[stage]);

         if (!prog->LinkStatus) {
            if (sh)
               _mesa_delete_shader(ctx, sh);
            goto done;
         }

         switch (stage) {
         case MESA_SHADER_VERTEX:
            validate_vertex_shader_executable(prog, sh, ctx);
            break;
         case MESA_SHADER_TESS_CTRL:
            /* nothing to be done */
            break;
         case MESA_SHADER_TESS_EVAL:
            validate_tess_eval_shader_executable(prog, sh, ctx);
            break;
         case MESA_SHADER_GEOMETRY:
            validate_geometry_shader_executable(prog, sh, ctx);
            break;
         case MESA_SHADER_FRAGMENT:
            validate_fragment_shader_executable(prog, sh);
            break;
         }
         if (!prog->LinkStatus) {
            if (sh)
               _mesa_delete_shader(ctx, sh);
            goto done;
         }

         _mesa_reference_shader(ctx, &prog->_LinkedShaders[stage], sh);
      }
   }

   if (num_shaders[MESA_SHADER_GEOMETRY] > 0) {
      prog->LastClipDistanceArraySize = prog->Geom.ClipDistanceArraySize;
      prog->LastCullDistanceArraySize = prog->Geom.CullDistanceArraySize;
   } else if (num_shaders[MESA_SHADER_TESS_EVAL] > 0) {
      prog->LastClipDistanceArraySize = prog->TessEval.ClipDistanceArraySize;
      prog->LastCullDistanceArraySize = prog->TessEval.CullDistanceArraySize;
   } else if (num_shaders[MESA_SHADER_VERTEX] > 0) {
      prog->LastClipDistanceArraySize = prog->Vert.ClipDistanceArraySize;
      prog->LastCullDistanceArraySize = prog->Vert.CullDistanceArraySize;
   } else {
      prog->LastClipDistanceArraySize = 0; /* Not used */
      prog->LastCullDistanceArraySize = 0; /* Not used */
   }

   /* Here begins the inter-stage linking phase.  Some initial validation is
    * performed, then locations are assigned for uniforms, attributes, and
    * varyings.
    */
   cross_validate_uniforms(prog);
   if (!prog->LinkStatus)
      goto done;

   unsigned first, last, prev;

   first = MESA_SHADER_STAGES;
   last = 0;

   /* Determine first and last stage. */
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      if (!prog->_LinkedShaders[i])
         continue;
      if (first == MESA_SHADER_STAGES)
         first = i;
      last = i;
   }

   num_explicit_uniform_locs = check_explicit_uniform_locations(ctx, prog);
   link_assign_subroutine_types(prog);

   if (!prog->LinkStatus)
      goto done;

   resize_tes_inputs(ctx, prog);

   /* Validate the inputs of each stage with the output of the preceding
    * stage.
    */
   prev = first;
   for (unsigned i = prev + 1; i <= MESA_SHADER_FRAGMENT; i++) {
      if (prog->_LinkedShaders[i] == NULL)
         continue;

      validate_interstage_inout_blocks(prog, prog->_LinkedShaders[prev],
                                       prog->_LinkedShaders[i]);
      if (!prog->LinkStatus)
         goto done;

      cross_validate_outputs_to_inputs(prog,
                                       prog->_LinkedShaders[prev],
                                       prog->_LinkedShaders[i]);
      if (!prog->LinkStatus)
         goto done;

      prev = i;
   }

   /* Cross-validate uniform blocks between shader stages */
   validate_interstage_uniform_blocks(prog, prog->_LinkedShaders,
                                      MESA_SHADER_STAGES);
   if (!prog->LinkStatus)
      goto done;

   for (unsigned int i = 0; i < MESA_SHADER_STAGES; i++) {
      if (prog->_LinkedShaders[i] != NULL)
         lower_named_interface_blocks(mem_ctx, prog->_LinkedShaders[i]);
   }

   /* Implement the GLSL 1.30+ rule for discard vs infinite loops Do
    * it before optimization because we want most of the checks to get
    * dropped thanks to constant propagation.
    *
    * This rule also applies to GLSL ES 3.00.
    */
   if (max_version >= (prog->IsES ? 300 : 130)) {
      struct gl_shader *sh = prog->_LinkedShaders[MESA_SHADER_FRAGMENT];
      if (sh) {
	 lower_discard_flow(sh->ir);
      }
   }

   if (prog->SeparateShader)
      disable_varying_optimizations_for_sso(prog);

   /* Process UBOs */
   if (!interstage_cross_validate_uniform_blocks(prog, false))
      goto done;

   /* Process SSBOs */
   if (!interstage_cross_validate_uniform_blocks(prog, true))
      goto done;

   /* Do common optimization before assigning storage for attributes,
    * uniforms, and varyings.  Later optimization could possibly make
    * some of that unused.
    */
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      if (prog->_LinkedShaders[i] == NULL)
	 continue;

      detect_recursion_linked(prog, prog->_LinkedShaders[i]->ir);
      if (!prog->LinkStatus)
	 goto done;

      if (ctx->Const.ShaderCompilerOptions[i].LowerCombinedClipCullDistance) {
         lower_clip_cull_distance(prog, prog->_LinkedShaders[i]);
      }

      if (ctx->Const.LowerTessLevel) {
         lower_tess_level(prog->_LinkedShaders[i]);
      }

      while (do_common_optimization(prog->_LinkedShaders[i]->ir, true, false,
                                    &ctx->Const.ShaderCompilerOptions[i],
                                    ctx->Const.NativeIntegers))
	 ;

      lower_const_arrays_to_uniforms(prog->_LinkedShaders[i]->ir);
   }

   /* Validation for special cases where we allow sampler array indexing
    * with loop induction variable. This check emits a warning or error
    * depending if backend can handle dynamic indexing.
    */
   if ((!prog->IsES && prog->Version < 130) ||
       (prog->IsES && prog->Version < 300)) {
      if (!validate_sampler_array_indexing(ctx, prog))
         goto done;
   }

   /* Check and validate stream emissions in geometry shaders */
   validate_geometry_shader_emissions(ctx, prog);

   /* Mark all generic shader inputs and outputs as unpaired. */
   for (unsigned i = MESA_SHADER_VERTEX; i <= MESA_SHADER_FRAGMENT; i++) {
      if (prog->_LinkedShaders[i] != NULL) {
         link_invalidate_variable_locations(prog->_LinkedShaders[i]->ir);
      }
   }

   prev = first;
   for (unsigned i = prev + 1; i <= MESA_SHADER_FRAGMENT; i++) {
      if (prog->_LinkedShaders[i] == NULL)
         continue;

      match_explicit_outputs_to_inputs(prog->_LinkedShaders[prev],
                                       prog->_LinkedShaders[i]);
      prev = i;
   }

   if (!assign_attribute_or_color_locations(prog, &ctx->Const,
                                            MESA_SHADER_VERTEX)) {
      goto done;
   }

   if (!assign_attribute_or_color_locations(prog, &ctx->Const,
                                            MESA_SHADER_FRAGMENT)) {
      goto done;
   }

   /* From the ARB_enhanced_layouts spec:
    *
    *    "If the shader used to record output variables for transform feedback
    *    varyings uses the "xfb_buffer", "xfb_offset", or "xfb_stride" layout
    *    qualifiers, the values specified by TransformFeedbackVaryings are
    *    ignored, and the set of variables captured for transform feedback is
    *    instead derived from the specified layout qualifiers."
    */
   for (int i = MESA_SHADER_FRAGMENT - 1; i >= 0; i--) {
      /* Find last stage before fragment shader */
      if (prog->_LinkedShaders[i]) {
         has_xfb_qualifiers =
            process_xfb_layout_qualifiers(mem_ctx, prog->_LinkedShaders[i],
                                          &num_tfeedback_decls,
                                          &varying_names);
         break;
      }
   }

   if (!has_xfb_qualifiers) {
      num_tfeedback_decls = prog->TransformFeedback.NumVarying;
      varying_names = prog->TransformFeedback.VaryingNames;
   }

   if (num_tfeedback_decls != 0) {
      /* From GL_EXT_transform_feedback:
       *   A program will fail to link if:
       *
       *   * the <count> specified by TransformFeedbackVaryingsEXT is
       *     non-zero, but the program object has no vertex or geometry
       *     shader;
       */
      if (first >= MESA_SHADER_FRAGMENT) {
         linker_error(prog, "Transform feedback varyings specified, but "
                      "no vertex, tessellation, or geometry shader is "
                      "present.\n");
         goto done;
      }

      tfeedback_decls = ralloc_array(mem_ctx, tfeedback_decl,
                                     num_tfeedback_decls);
      if (!parse_tfeedback_decls(ctx, prog, mem_ctx, num_tfeedback_decls,
                                 varying_names, tfeedback_decls))
         goto done;
   }

   /* If there is no fragment shader we need to set transform feedback.
    *
    * For SSO we also need to assign output locations.  We assign them here
    * because we need to do it for both single stage programs and multi stage
    * programs.
    */
   if (last < MESA_SHADER_FRAGMENT &&
       (num_tfeedback_decls != 0 || prog->SeparateShader)) {
      const uint64_t reserved_out_slots =
         reserved_varying_slot(prog->_LinkedShaders[last], ir_var_shader_out);
      if (!assign_varying_locations(ctx, mem_ctx, prog,
                                    prog->_LinkedShaders[last], NULL,
                                    num_tfeedback_decls, tfeedback_decls,
                                    reserved_out_slots))
         goto done;
   }

   if (last <= MESA_SHADER_FRAGMENT) {
      /* Remove unused varyings from the first/last stage unless SSO */
      remove_unused_shader_inputs_and_outputs(prog->SeparateShader,
                                              prog->_LinkedShaders[first],
                                              ir_var_shader_in);
      remove_unused_shader_inputs_and_outputs(prog->SeparateShader,
                                              prog->_LinkedShaders[last],
                                              ir_var_shader_out);

      /* If the program is made up of only a single stage */
      if (first == last) {

         gl_shader *const sh = prog->_LinkedShaders[last];
         if (prog->SeparateShader) {
            const uint64_t reserved_slots =
               reserved_varying_slot(sh, ir_var_shader_in);

            /* Assign input locations for SSO, output locations are already
             * assigned.
             */
            if (!assign_varying_locations(ctx, mem_ctx, prog,
                                          NULL /* producer */,
                                          sh /* consumer */,
                                          0 /* num_tfeedback_decls */,
                                          NULL /* tfeedback_decls */,
                                          reserved_slots))
               goto done;
         }

         do_dead_builtin_varyings(ctx, NULL, sh, 0, NULL);
         do_dead_builtin_varyings(ctx, sh, NULL, num_tfeedback_decls,
                                  tfeedback_decls);
      } else {
         /* Linking the stages in the opposite order (from fragment to vertex)
          * ensures that inter-shader outputs written to in an earlier stage
          * are eliminated if they are (transitively) not used in a later
          * stage.
          */
         int next = last;
         for (int i = next - 1; i >= 0; i--) {
            if (prog->_LinkedShaders[i] == NULL && i != 0)
               continue;

            gl_shader *const sh_i = prog->_LinkedShaders[i];
            gl_shader *const sh_next = prog->_LinkedShaders[next];

            const uint64_t reserved_out_slots =
               reserved_varying_slot(sh_i, ir_var_shader_out);
            const uint64_t reserved_in_slots =
               reserved_varying_slot(sh_next, ir_var_shader_in);

            if (!assign_varying_locations(ctx, mem_ctx, prog, sh_i, sh_next,
                      next == MESA_SHADER_FRAGMENT ? num_tfeedback_decls : 0,
                      tfeedback_decls,
                      reserved_out_slots | reserved_in_slots))
               goto done;

            do_dead_builtin_varyings(ctx, sh_i, sh_next,
                      next == MESA_SHADER_FRAGMENT ? num_tfeedback_decls : 0,
                      tfeedback_decls);

            /* This must be done after all dead varyings are eliminated. */
            if (sh_i != NULL) {
               unsigned slots_used = _mesa_bitcount_64(reserved_out_slots);
               if (!check_against_output_limit(ctx, prog, sh_i, slots_used)) {
                  goto done;
               }
            }

            unsigned slots_used = _mesa_bitcount_64(reserved_in_slots);
            if (!check_against_input_limit(ctx, prog, sh_next, slots_used))
               goto done;

            next = i;
         }
      }
   }

   if (!store_tfeedback_info(ctx, prog, num_tfeedback_decls, tfeedback_decls,
                             has_xfb_qualifiers))
      goto done;

   update_array_sizes(prog);
   link_assign_uniform_locations(prog, ctx->Const.UniformBooleanTrue,
                                 num_explicit_uniform_locs,
                                 ctx->Const.MaxUserAssignableUniformLocations);
   link_assign_atomic_counter_resources(ctx, prog);
   store_fragdepth_layout(prog);

   link_calculate_subroutine_compat(prog);
   check_resources(ctx, prog);
   check_subroutine_resources(prog);
   check_image_resources(ctx, prog);
   link_check_atomic_counter_resources(ctx, prog);

   if (!prog->LinkStatus)
      goto done;

   /* OpenGL ES < 3.1 requires that a vertex shader and a fragment shader both
    * be present in a linked program. GL_ARB_ES2_compatibility doesn't say
    * anything about shader linking when one of the shaders (vertex or
    * fragment shader) is absent. So, the extension shouldn't change the
    * behavior specified in GLSL specification.
    *
    * From OpenGL ES 3.1 specification (7.3 Program Objects):
    *     "Linking can fail for a variety of reasons as specified in the
    *     OpenGL ES Shading Language Specification, as well as any of the
    *     following reasons:
    *
    *     ...
    *
    *     * program contains objects to form either a vertex shader or
    *       fragment shader, and program is not separable, and does not
    *       contain objects to form both a vertex shader and fragment
    *       shader."
    *
    * However, the only scenario in 3.1+ where we don't require them both is
    * when we have a compute shader. For example:
    *
    * - No shaders is a link error.
    * - Geom or Tess without a Vertex shader is a link error which means we
    *   always require a Vertex shader and hence a Fragment shader.
    * - Finally a Compute shader linked with any other stage is a link error.
    */
   if (!prog->SeparateShader && ctx->API == API_OPENGLES2 &&
       num_shaders[MESA_SHADER_COMPUTE] == 0) {
      if (prog->_LinkedShaders[MESA_SHADER_VERTEX] == NULL) {
	 linker_error(prog, "program lacks a vertex shader\n");
      } else if (prog->_LinkedShaders[MESA_SHADER_FRAGMENT] == NULL) {
	 linker_error(prog, "program lacks a fragment shader\n");
      }
   }

   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      if (prog->_LinkedShaders[i] == NULL)
	 continue;

      const struct gl_shader_compiler_options *options =
         &ctx->Const.ShaderCompilerOptions[i];

      if (options->LowerBufferInterfaceBlocks)
         lower_ubo_reference(prog->_LinkedShaders[i],
                             options->ClampBlockIndicesToArrayBounds);

      if (options->LowerShaderSharedVariables)
         lower_shared_reference(prog->_LinkedShaders[i],
                                &prog->Comp.SharedSize);

      lower_vector_derefs(prog->_LinkedShaders[i]);
      do_vec_index_to_swizzle(prog->_LinkedShaders[i]->ir);
   }

done:
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      free(shader_list[i]);
      if (prog->_LinkedShaders[i] == NULL)
	 continue;

      /* Do a final validation step to make sure that the IR wasn't
       * invalidated by any modifications performed after intrastage linking.
       */
      validate_ir_tree(prog->_LinkedShaders[i]->ir);

      /* Retain any live IR, but trash the rest. */
      reparent_ir(prog->_LinkedShaders[i]->ir, prog->_LinkedShaders[i]->ir);

      /* The symbol table in the linked shaders may contain references to
       * variables that were removed (e.g., unused uniforms).  Since it may
       * contain junk, there is no possible valid use.  Delete it and set the
       * pointer to NULL.
       */
      delete prog->_LinkedShaders[i]->symbols;
      prog->_LinkedShaders[i]->symbols = NULL;
   }

   ralloc_free(mem_ctx);
}
