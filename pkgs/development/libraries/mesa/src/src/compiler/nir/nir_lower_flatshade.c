/*
 * Copyright © 2015 Red Hat
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "nir.h"
#include "nir_builder.h"

static bool
lower_input(nir_shader *shader, nir_variable *var)
{
   if (var->data.interpolation == INTERP_MODE_NONE &&
       (var->data.location == VARYING_SLOT_COL0 ||
        var->data.location == VARYING_SLOT_COL1 ||
        var->data.location == VARYING_SLOT_BFC0 ||
        var->data.location == VARYING_SLOT_BFC1))
      var->data.interpolation = INTERP_MODE_FLAT;
   return true;
}

bool
nir_lower_flatshade(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_shader_in_variable(var, shader) {
      progress |= lower_input(shader, var);
   }

   /* Interpolation doesn't affect any metadata */
   nir_shader_preserve_all_metadata(shader);
   return progress;
}