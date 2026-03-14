#ifndef INCLUDES_TARANTOOL_BOX_VY_RUN_PROTO_H
#define INCLUDES_TARANTOOL_BOX_VY_RUN_PROTO_H

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct key_def;
struct vy_page_info;
struct xrow_header;

int
vy_page_info_decode(struct vy_page_info *page, const struct xrow_header *xrow,
		    struct key_def *cmp_def, const char *filename);

int
vy_page_info_encode(const struct vy_page_info *page_info,
		    struct xrow_header *xrow);

struct vy_page_info *
vy_page_info_new(void);

void
vy_page_info_destroy(struct vy_page_info *page_info);

void
vy_page_info_delete(struct vy_page_info *page_info);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_BOX_VY_RUN_PROTO_H */
