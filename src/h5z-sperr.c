/*
 * This file contains the SPERR filter class definition, H5Z_SPERR_class_t,
 * and necessary functions required by the HDF5 plugin architecture:
 * - can_apply()
 * - set_local()
 * - filter()
 * - get_plugin_info()
 * - get_plugin_type()
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <H5PLextern.h>
#include <hdf5.h>

#include <SPERR_C_API.h>
#include "h5z-sperr.h"
#include "h5zsperr_helper.h"

#ifndef NDEBUG
#include <stdio.h>
#endif

static htri_t H5Z_can_apply_sperr(hid_t dcpl_id, hid_t type_id, hid_t space_id)
{
  /*
   * 	dcpl_id	  Dataset creation property list identifier
   * 	type_id	  Datatype identifier
   * 	space_id  Dataspace identifier
   */

  /* Get datatype class. Fail if not floats. */
  if (H5Tget_class(type_id) != H5T_FLOAT) {
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADTYPE,
            "bad data type. Only floats are supported in H5Z-SPERR");
    return 0;
  }

  /* Get the dataspace rank. Fail if not 2, 3, or 4. */
  int ndims = H5Sget_simple_extent_ndims(space_id);
  if (ndims < 2 || ndims > 4) {
#ifndef NDEBUG
    printf("%s: %d, ndims = %d\n", __FILE__, __LINE__, ndims);
#endif
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADTYPE,
            "bad dataspace ranks. Only rank==2, rank==3, or rank==4 with the time dimension==1 are "
            "supported in H5Z-SPERR");
    return 0;
  }

  /* Get the dataspace dimension. */
  hsize_t dspace_dims[4] = {0, 0, 0, 0};
  ndims = H5Sget_simple_extent_dims(space_id, dspace_dims, NULL);

  /* Chunks have to be 2D, 3D, or 4D as well. */
  hsize_t chunks[4] = {0, 0, 0, 0};
  ndims = H5Pget_chunk(dcpl_id, 4, chunks);
  if (ndims < 2 || ndims > 4) {
#ifndef NDEBUG
    printf("%s: %d, ndims = %d\n", __FILE__, __LINE__, ndims);
#endif
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADTYPE,
            "bad chunk ranks. Only rank==2, rank==3, or rank==4 with the time dimension==1 are "
            "supported in H5Z-SPERR");
    return 0;
  }

  /* Dataspace dimension must be divisible by the chunk dimension. */
  for (int i = 0; i < ndims; i++)
    if (dspace_dims[i] % chunks[i]) {
#ifndef NDEBUG
      printf("%s: %d, dataspace dim = %llu, chunk dim = %llu\n", __FILE__, __LINE__, dspace_dims[i],
             chunks[i]);
#endif
      H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADTYPE,
              "bad chunk size. The dataspace dimensions must be divisible by the chunk dimension");
      return 0;
    }

  /* Find out the real dimension (of each chunk). */
  int real_dims = 0;
  for (int i = 0; i < 4; i++)
    if (chunks[i] > 1)
      real_dims++;
  if (real_dims < 2 || real_dims > 3) {
#ifndef NDEBUG
    printf("%s: %d, real_dims = %d\n", __FILE__, __LINE__, real_dims);
#endif
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADTYPE,
            "bad chunk dimensions: only true 2D slices or 3D volumes are supported in H5Z-SPERR");
    return 0;
  }

  /* Real chunk dimensions must be at least 9 */
  for (int i = 0; i < ndims; i++) {
    if (chunks[i] > 1 && chunks[i] < 9) {
#ifndef NDEBUG
      printf("%s: %d, chunks[%d] = %llu\n", __FILE__, __LINE__, i, chunks[i]);
#endif
      H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADTYPE,
              "bad chunk dimensions: any dimension must be at least 9. (may relax this requirement "
              "in the future)");
      return 0;
    }
  }

  return 1;
}

static herr_t H5Z_set_local_sperr(hid_t dcpl_id, hid_t type_id, hid_t space_id)
{
  /*
   * 	dcpl_id	  Dataset creation property list identifier
   * 	type_id	  Datatype identifier
   * 	space_id  Dataspace identifier
   */

  /*
   * Get the user-specified parameters. It has mandatory and optional fields.
   * -- One integer (mandatory): compression mode, quality, rank swap
   * -- One integer (optional) : missing value mode
   * -- One/two integers (optional): a float or double specifying the missing value
   */
  size_t user_cd_nelem = 4; /* the maximum possible number */
  unsigned int user_cd_values[4] = {0, 0, 0, 0};
  char name[16];
  for (size_t i = 0; i < 16; i++)
    name[i] = ' ';
  unsigned int flags = 0, filter_config = 0;
  herr_t status = H5Pget_filter_by_id(dcpl_id, H5Z_FILTER_SPERR, &flags, &user_cd_nelem,
                                      user_cd_values, 16, name, &filter_config);

  /*
   * `missing_val_mode` meaning:
   * 0: no missing value
   * 1: any NAN is a missing value
   * 2: any value where abs(value) >= 1e35 is a missing value.
   * 3: use a single 32-bit float as the missing value.
   * 4: use a single 64-bit double as the missing value.
   */
  int missing_val_mode = 0;
  float missing_val_f = 0.0f;
  double missing_val_d = 0.0;
  if (user_cd_nelem == 1) {}  /* not providing missing value mode */
  else if (user_cd_nelem == 2) {
    missing_val_mode = user_cd_values[1];
    if (missing_val_mode > 2) {
#ifndef NDEBUG
      printf("%s: %d, user_cd_nelem = %lu\n", __FILE__, __LINE__, user_cd_nelem);
#endif
      H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADSIZE,
              "User cd_values[] isn't valid.");
      return -1;
    }
  }
  else if (user_cd_nelem == 3) {
    missing_val_mode = user_cd_values[1];
    if (missing_val_mode != 3) {
#ifndef NDEBUG
      printf("%s: %d, user_cd_nelem = %lu\n", __FILE__, __LINE__, user_cd_nelem);
#endif
      H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADSIZE,
              "User cd_values[] isn't valid.");
      return -1;
    }
    memcpy(&missing_val_f, &user_cd_values[2], sizeof(missing_val_f));
  }
  else if (user_cd_nelem == 4) {
    missing_val_mode = user_cd_values[1];
    if (missing_val_mode != 4) {
#ifndef NDEBUG
      printf("%s: %d, user_cd_nelem = %lu\n", __FILE__, __LINE__, user_cd_nelem);
#endif
      H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADSIZE,
              "User cd_values[] isn't valid.");
      return -1;
    }
    memcpy(&missing_val_d, &user_cd_values[2], sizeof(missing_val_d));
  }
  else {
#ifndef NDEBUG
    printf("%s: %d, user_cd_nelem = %lu\n", __FILE__, __LINE__, user_cd_nelem);
#endif
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADSIZE,
            "User cd_values[] has more than 4 elements.");
    return -1;
  }

  /* Get the datatype size. It must be 4 or 8, since the float type is verified by `can_apply`. */
  int is_float = 1;
  if (H5Tget_size(type_id) == 8)
    is_float = 0; /* !is_float, i.e. double */
  else
    assert(H5Tget_size(type_id) == 4);

  /* Get chunk sizes. */
  hsize_t chunks[4] = {0, 0, 0, 0};
  int ndims = H5Pget_chunk(dcpl_id, 4, chunks);
  int real_dims = 0;
  for (int i = 0; i < 4; i++)
    if (chunks[i] > 1)
      real_dims++;
  assert(real_dims == 2 || real_dims == 3);

  /*
   * Assemble the meta info to be stored.
   * [0]  : 2D/3D, float/double, missing_val specifics
   * [1]  : compression specifics (user input)
   * [2-3]: (dimx, dimy) in 2D cases.
   * [2-4]: (dimx, dimy, dimz) in 3D cases.
   * Followed by 0, 1, or 2 integers storing the exact missing value.
   */
  unsigned int cd_values[7] = {0, 0, 0, 0, 0, 0, 0};
  cd_values[0] = h5zsperr_pack_extra_info(real_dims, is_float, missing_val_mode);
  cd_values[1] = user_cd_values[0];
  int i1 = 2, i2 = 0;
  while (i2 < 4) {
    if (chunks[i2] > 1)
      cd_values[i1++] = (unsigned int)chunks[i2];
    i2++;
  }

  /* figure out the length of cd_values[] */
  size_t cd_nelems = real_dims == 2 ? 4 : 5;
  if (missing_val_mode == 3) {  /* a specific float */
    cd_nelems += 1;
    memcpy(&cd_values[i1], &missing_val_f, sizeof(missing_val_f));
  }
  else if (missing_val_mode == 4) {  /* a specific double */
    cd_nelems += 2;
    memcpy(&cd_values[i1], &missing_val_d, sizeof(missing_val_d));
  }

  H5Pmodify_filter(dcpl_id, H5Z_FILTER_SPERR, H5Z_FLAG_MANDATORY, cd_nelems, cd_values);

  return 0;
}

static size_t H5Z_filter_sperr(unsigned int flags,
                               size_t cd_nelmts,
                               const unsigned int cd_values[],
                               size_t nbytes,
                               size_t* buf_size,
                               void** buf)
{
  /* Extract info from cd_values[] */
  int rank = 0, is_float = 0, missing_val_mode = 0;
  h5zsperr_unpack_extra_info(cd_values[0], &rank, &is_float, &missing_val_mode);
  assert(rank == 2 || rank == 3);
  assert(is_float == 0 || is_float == 1);
  assert(missing_val_mode >= 0 && missing_val_mode <= 4);
  if (missing_val_mode <= 2)
    assert(cd_nelmts == (rank == 2 ? 4 : 5));
  else if (missing_val_mode == 3)
    assert(cd_nelmts == (rank == 2 ? 5 : 6));
  else  /* missing_val_mode == 4 */
    assert(cd_nelmts == (rank == 2 ? 6 : 7));

  int comp_mode = 0, swap = 0;
  double quality = 0.0;
  H5Z_SPERR_decode_cd_values(cd_values[1], &comp_mode, &quality, &swap);
  unsigned int dims[3] = {cd_values[2], cd_values[3], rank == 2 ? 1 : cd_values[4]};
  if (swap) {
    unsigned int tmp = dims[0];
    if (rank == 2) {
      dims[0] = dims[1];
      dims[1] = tmp;
    }
    else {
      dims[0] = dims[2];
      dims[2] = tmp;
    }
  }

  int cd_val_offset = 4;
  if (rank == 3)
    cd_val_offset = 5;

  float missing_val_f = 0.f;
  double missing_val_d = 0.0;
  if (missing_val_mode == 3)
    memcpy(&missing_val_f, &cd_values[cd_val_offset], sizeof(missing_val_f));
  else if (missing_val_mode == 4)
    memcpy(&missing_val_d, &cd_values[cd_val_offset], sizeof(missing_val_d));

  /* Decompression */
  if (flags & H5Z_FLAG_REVERSE) {
    void* dst = NULL; /* buffer to hold the decompressed data */
    int ret = 0;
    if (rank == 2)
      ret = sperr_decomp_2d(*buf, nbytes, is_float, dims[0], dims[1], &dst);
    else {
      size_t dimx = 0, dimy = 0, dimz = 0;
      ret = sperr_decomp_3d(*buf, nbytes, is_float, 1, &dimx, &dimy, &dimz, &dst);
    }
    if (ret != 0) {
      if (dst) {
        free(dst); /* allocated by SPERR, using malloc() */
        dst = NULL;
      }
      H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADVALUE,
              "SPERR decompression failed.");
      return 0;
    }

    size_t dst_len = (is_float ? 4ul : 8ul) * dims[0] * dims[1] * dims[2];
    if (dst_len <= *buf_size) { /* Re-use the input buffer */
      memcpy(*buf, dst, dst_len);
      free(dst); /* allocated by SPERR, using malloc() */
      dst = NULL;
    }
    else {                 /* Point to the new buffer */
      H5free_memory(*buf); /* allocated by HDF5 */
      *buf = dst;
      *buf_size = dst_len;
    }

    return dst_len;

  } /* Finish Decompression */
  else { /* Compression */

    /* Sanity check on the data size. */
    if ((is_float ? 4ul : 8ul) * dims[0] * dims[1] * dims[2] != nbytes) {
      H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADSIZE,
              "Compression: input buffer len isn't right.");
      return 0;
    }

    void* dst = NULL; /* buffer to hold the compressed bitstream */
    size_t dst_len = 0;
    int ret = 0;

    if (rank == 2)
      ret = sperr_comp_2d(*buf, is_float, dims[0], dims[1], comp_mode, quality, 0, &dst, &dst_len);
    else
      ret = sperr_comp_3d(*buf, is_float, dims[0], dims[1], dims[2], dims[0], dims[1], dims[2],
                          comp_mode, quality, 1, &dst, &dst_len);
    if (ret != 0) {
      if (dst) {
        free(dst); /* allocated by SPERR, using malloc() */
        dst = NULL;
      }
      H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADVALUE,
              "SPERR compression failed.");
      return 0;
    }

    if (dst_len <= *buf_size) { /* Re-use the input buffer */
      memcpy(*buf, dst, dst_len);
      free(dst); /* allocated by SPERR, using malloc() */
      dst = NULL;
    }
    else {                 /* Point to the new buffer */
      H5free_memory(*buf); /* allocated by HDF5 */
      *buf = dst;
      *buf_size = dst_len;
    }

    return dst_len;

  } /* Finish compression */
}

const H5Z_class2_t H5Z_SPERR_class_t = {H5Z_CLASS_T_VERS, /* H5Z_class_t version */
                                        H5Z_FILTER_SPERR, /* Filter id number    */
                                        1,                /* encoder_present flag (set to true) */
                                        1,                /* decoder_present flag (set to true) */
                                        "H5Z-SPERR",      /* Filter name for debugging  */
                                        H5Z_can_apply_sperr, /* The "can apply" callback   */
                                        H5Z_set_local_sperr, /* The "set local" callback   */
                                        H5Z_filter_sperr};   /* The actual filter function */

const void* H5PLget_plugin_info()
{
  return &H5Z_SPERR_class_t;
}

H5PL_type_t H5PLget_plugin_type()
{
  return H5PL_TYPE_FILTER;
}
