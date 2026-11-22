/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "gl_query.hh"

namespace blender::gpu {

#define QUERY_CHUNCK_LEN 256

GLQueryPool::~GLQueryPool()
{
  glDeleteQueries(query_ids_.size(), query_ids_.data());
}

void GLQueryPool::init(GPUQueryType type)
{
  BLI_assert(initialized_ == false);
  initialized_ = true;
  type_ = type;
  gl_type_ = to_gl(type);
  query_issued_ = 0;
}

#if 0 /* TODO: to avoid realloc of permanent query pool. */
void GLQueryPool::reset(GPUQueryType type)
{
  initialized_ = false;
}
#endif

void GLQueryPool::begin_query()
{
  /* TODO: add assert about expected usage. */
  while (query_issued_ >= query_ids_.size()) {
    int64_t prev_size = query_ids_.size();
    int64_t chunk_size = prev_size == 0 ? query_ids_.capacity() : QUERY_CHUNCK_LEN;
    query_ids_.resize(prev_size + chunk_size);
    glGenQueries(chunk_size, &query_ids_[prev_size]);
  }
  glBeginQuery(gl_type_, query_ids_[query_issued_++]);
}

void GLQueryPool::end_query()
{
  /* TODO: add assert about expected usage. */
  glEndQuery(gl_type_);
}

void GLQueryPool::get_occlusion_result(MutableSpan<uint32_t> r_values)
{
  BLI_assert(r_values.size() == query_issued_);

  for (int i = 0; i < query_issued_; i++) {
    /* NOTE: This is a sync point. */
    glGetQueryObjectuiv(query_ids_[i], GL_QUERY_RESULT, &r_values[i]);
  }
}

int GLQueryPool::timestamp_write()
{
  if (query_issued_ == query_ids_.size()) {
    query_ids_.resize(query_issued_ + QUERY_CHUNCK_LEN);
    glGenQueries(QUERY_CHUNCK_LEN, &query_ids_[query_issued_]);
  }
  glQueryCounter(query_ids_[query_issued_], GL_TIMESTAMP);
  return query_issued_++;
}

bool GLQueryPool::timestamp_duration(int begin, int end, double &ms) const
{
  GLuint64 start, finish;
  glGetQueryObjectui64v(query_ids_[end], GL_QUERY_RESULT, &finish);
  glGetQueryObjectui64v(query_ids_[begin], GL_QUERY_RESULT, &start);
  ms = double(finish - start) * 1e-6;
  return true;
}

}  // namespace blender::gpu
