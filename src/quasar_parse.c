/*-------------------------------------------------------------------------
 *
 * Quasar Foreign Data Wrapper for PostgreSQL
 *
 * Copyright (c) 2015 SlamData
 *
 * This software is released under the PostgreSQL Licence
 *
 * Author: Jon Eisen <jon@joneisen.works>
 *
 * IDENTIFICATION
 *            quasar_fdw/src/quasar_parse.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "quasar_fdw.h"

#include "yajl/yajl_parse.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "common/fe_memutils.h"
#include "utils/syscache.h"


#define YAJL_OK 1
#define YAJL_CANCEL 0

#define NO_COLUMN -1

#define TOP_LEVEL 0
#define COLUMN_LEVEL 1

typedef struct parser {
    TupleTableSlot *slot;
    struct QuasarTable *table;
    size_t cur_col;
    bool record_complete;
    int level;
    StringInfoData structure;
} parser;

/* Utilities */
regproc getTypeFunction(Oid pgtype);
static struct QuasarColumn *get_column(parser *p);
static bool is_array_type(parser *p);
static bool is_json_type(parser *p);
static void appendCommaIf(parser *p);
static void store_datum(parser *p, Datum dat, const char *fmt);
static void store_null(parser *p);

/* Alloc callbacks */
void *yajl_palloc(void *ctx, size_t sz);
void *yajl_repalloc(void *ctx, void *ptr, size_t sz);
void yajl_pfree(void *ctx, void *ptr);

/* Parse callbacks */
static int cb_null(void * ctx);
static int cb_boolean(void * ctx, int boolean);
static int cb_string(void * ctx, const unsigned char * value, size_t len);
static int cb_number(void * ctx, const char * value, size_t len);
static int cb_map_key(void * ctx, const unsigned char * stringVal, size_t stringLen);
static int cb_start_map(void * ctx);
static int cb_end_map(void * ctx);
static int cb_start_array(void * ctx);
static int cb_end_array(void * ctx);


regproc getTypeFunction(Oid pgtype) {
    /* find the appropriate conversion function */
    regproc typinput;
    HeapTuple tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(pgtype));
    if (!HeapTupleIsValid(tuple))
    {
        elog(ERROR, "cache lookup failed for type %u", pgtype);
    }
    typinput = ((Form_pg_type)GETSTRUCT(tuple))->typinput;
    ReleaseSysCache(tuple);
    return typinput;
}

static struct QuasarColumn *get_column(parser *p) {
    if (p->cur_col != NO_COLUMN) {
        return p->table->cols[p->cur_col];
    } else {
        elog(ERROR, "Got a value when no column specified!");
    }
}

static bool is_array_type(parser *p) {
    switch(get_column(p)->pgtype) {
    case INT2ARRAYOID:
    case INT4ARRAYOID:
    case TEXTARRAYOID:
    case OIDARRAYOID:
    case FLOAT4ARRAYOID:
    case CSTRINGARRAYOID:
    case ANYARRAYOID:
        return true;
    default:
        return false;
    }
}

static bool is_json_type(parser *p) {
    switch(get_column(p)->pgtype) {
    case JSONOID:
    case JSONBOID:
        return true;
    default:
        return false;
    }
}

static void appendCommaIf(parser *p) {
    StringInfo s = &p->structure;
    if (s->len <= 0) return;
    if (is_array_type(p)) {
        switch(s->data[s->len - 1]) {
        case '{':
        case ',':
            return;
        default:
            appendStringInfoChar(s, ',');
        }
    } else if (is_json_type(p)) {
        switch(s->data[s->len - 1]) {
        case '{':
        case '[':
        case ':':
        case ',':
            return;
        default:
            appendStringInfoChar(s, ',');
        }
    } else {
        elog(ERROR, "Got illegal value for non-json non-array type");
    }
}

static void store_datum(parser *p, Datum dat, const char *fmt) {
    struct QuasarColumn *col = get_column(p);

    if (p->level == COLUMN_LEVEL) {
        /* If the column is set, lets put this in the tuple */
        switch (col->pgtype) {
        case BPCHAROID:
        case VARCHAROID:
        case TIMESTAMPOID:
        case TIMESTAMPTZOID:
        case INTERVALOID:
        case NUMERICOID:
            /* these functions require the type modifier */
            p->slot->tts_values[p->cur_col] =
                OidFunctionCall3(getTypeFunction(col->pgtype),
                                 dat,
                                 ObjectIdGetDatum(InvalidOid),
                                 Int32GetDatum(col->pgtypmod));
            break;
        default:
            /* the others don't */
            p->slot->tts_values[p->cur_col] =
                OidFunctionCall1(getTypeFunction(col->pgtype), dat);
        }
    } else if (p->level > COLUMN_LEVEL) {
        appendCommaIf(p);
        appendStringInfo(&p->structure, fmt, DatumGetCString(dat));
    }
}


static void store_null(parser *p) {
    /* Assert the column is valid */

    if (p->level > COLUMN_LEVEL) {
        appendCommaIf(p);
        appendStringInfo(&p->structure, "null");
    } else if (p->level == COLUMN_LEVEL) {
        /* If the column is set, lets put this in the tuple */
        p->slot->tts_values[p->cur_col] = PointerGetDatum(NULL);
        p->slot->tts_isnull[p->cur_col] = true;
    } else {
        elog(ERROR, "storing null when level is above columns");
    }
}

/* Yajl callbacks */
static int cb_null(void * ctx) {
    store_null((parser*) ctx);
    return YAJL_OK;
}

static int cb_boolean(void * ctx, int boolean) {
    store_datum((parser*) ctx, BoolGetDatum(boolean), "%s");
    return YAJL_OK;
}


static int cb_string(void * ctx,
                    const unsigned char * value,
                    size_t len) {
    store_datum((parser*) ctx, CStringGetDatum(pnstrdup((const char *)value, len)), "\"%s\"");
    return YAJL_OK;
}

static int cb_number(void * ctx,
                     const char * value,
                     size_t len) {
    store_datum((parser*) ctx, CStringGetDatum(pnstrdup(value, len)), "%s");
    return YAJL_OK;
}

static int cb_map_key(void * ctx,
                      const unsigned char * stringVal,
                      size_t stringLen) {
    parser *p = (parser*) ctx;
    size_t i;

    if (p->level == COLUMN_LEVEL) {
        /* Find the column */
        p->cur_col = NO_COLUMN;
        for (i = 0; i < p->table->ncols; ++i) {
            if (strncmp((const char *)stringVal,
                        p->table->cols[i]->pgname,
                        stringLen) == 0) {
                p->cur_col = i;
                break;
            }
        }
        if (p->cur_col == NO_COLUMN) {
            elog(ERROR, "Couldnt find column for returned JSON field: %s",
                 pnstrdup((const char *)stringVal, stringLen));
        }
    } else if (p->level > COLUMN_LEVEL) {
        if (is_json_type(p)) {
            appendCommaIf(p);
            char *s = pnstrdup((const char *)stringVal, stringLen);
            appendStringInfo(&p->structure, "\"%s\":", s);
            pfree(s);
        } else {
            elog(ERROR, "Got map key inside non-json type");
        }
    }
    return YAJL_OK;
}

static int cb_start_map(void * ctx) {
    parser *p = (parser*) ctx;
    if (p->level >= COLUMN_LEVEL) {
        if (is_json_type(p)) {
            appendCommaIf(p);
            appendStringInfoChar(&p->structure, '{');
        } else {
            elog(ERROR, "Got opening map inside non-json type");
        }
    }
    p->level++;
    return YAJL_OK;
}


static int cb_end_map(void * ctx) {
    parser *p = (parser*) ctx;
    if (p->level > COLUMN_LEVEL) {
        if (is_json_type(p)) {
            appendStringInfoChar(&p->structure, '}');
        } else {
            elog(ERROR, "Got closing map inside non-json type");
        }
    }

    p->level--;

    if (p->level == COLUMN_LEVEL) {
        elog(DEBUG2, "Parsed value for json structure: %s", p->structure.data);
        store_datum(p, CStringGetDatum(pstrdup(p->structure.data)), "%s");
        resetStringInfo(&p->structure);
    } else if (p->level == TOP_LEVEL) {
        p->record_complete = true;
        return YAJL_CANCEL;
    }
    return YAJL_OK;
}

static int cb_start_array(void * ctx) {
    parser *p = (parser*) ctx;
    if (p->level >= COLUMN_LEVEL) {
        if (is_array_type(p)) {
            if (p->level > COLUMN_LEVEL)
                appendCommaIf(p);
            appendStringInfoChar(&p->structure, '{');
        } else if (is_json_type(p)) {
            appendCommaIf(p);
            appendStringInfoChar(&p->structure, '[');
        } else {
            elog(ERROR, "Got opening array inside non-array, non-json type");
        }
    }
    p->level++;
    return YAJL_OK;
}

static int cb_end_array(void * ctx) {
    parser *p = (parser*) ctx;
    if (p->level > COLUMN_LEVEL) {
        if (is_array_type(p)) {
            appendStringInfo(&p->structure, "}");
        } else if (is_json_type(p)) {
            appendStringInfo(&p->structure, "]");
        } else {
            elog(ERROR, "Got closing array inside non-array, non-json type");
        }
    }

    p->level--;

    if (p->level == COLUMN_LEVEL) {
        elog(DEBUG2, "Parsed value for deep structure: %s", p->structure.data);
        store_datum(p, CStringGetDatum(pstrdup(p->structure.data)), "%s");
        resetStringInfo(&p->structure);
    }
    return YAJL_OK;
}

static yajl_callbacks callbacks = {
    cb_null,
    cb_boolean,
    NULL,
    NULL,
    cb_number,
    cb_string,
    cb_start_map,
    cb_map_key,
    cb_end_map,
    cb_start_array,
    cb_end_array
};

/* Yajl alloc functions use palloc */
void *yajl_palloc(void *ctx, size_t sz) {
    return palloc(sz);
}
void *yajl_repalloc(void *ctx, void *ptr, size_t sz) {
    if (ptr == NULL)
        return palloc(sz);
    else
        return repalloc(ptr, sz);
}
void yajl_pfree(void *ctx, void *ptr) {
    if (ptr != NULL)
        return pfree(ptr);
}
static yajl_alloc_funcs allocs = {yajl_palloc, yajl_repalloc, yajl_pfree, NULL};


void quasar_parse_alloc(quasar_parse_context *ctx, struct QuasarTable *table) {
    elog(DEBUG1, "entering function %s", __func__);

    parser *p = palloc(sizeof(parser));
    p->cur_col = NO_COLUMN;
    p->table = table;
    p->level = TOP_LEVEL;
    p->record_complete = false;
    initStringInfo(&p->structure);
    ctx->p = p;
    /* ctx->handle = yajl_alloc(&callbacks, &allocs, p); */
    /* yajl_config(ctx->handle, yajl_allow_multiple_values, 1); */

}
void quasar_parse_free(quasar_parse_context *ctx) {
    elog(DEBUG1, "entering function %s", __func__);

    /* yajl_free(ctx->handle); */
    pfree(((parser*)ctx->p)->structure.data);
    pfree(ctx->p);
}

bool quasar_parse(quasar_parse_context *ctx,
                  const char *buffer,
                  size_t *buf_loc,
                  size_t buf_size) {
    elog(DEBUG1, "entering function %s", __func__);

    bool found = false;
    parser *p = (parser*) ctx->p;
    yajl_status status;
    yajl_handle hand = yajl_alloc(&callbacks, &allocs, ctx->p);

    while (!found && *buf_loc < buf_size) {
        /* Response is formed as many json objects
         * So we can just parse until a full one is completed
         * and pick it up where we left off next time.
         * We do this by only parsing until '}' */

        status = yajl_parse(hand,
                            (const unsigned char *)buffer + *buf_loc,
                            buf_size - *buf_loc);
        if (status == yajl_status_error) {
            unsigned char *errstr =
                yajl_get_error(hand, 1,
                               (const unsigned char *)buffer + *buf_loc,
                               buf_size - *buf_loc);
            elog(ERROR, "Error parsing json: %s", errstr);
            yajl_free_error(hand, errstr); /* Never executed... */
        }
        elog(DEBUG1, "Consumed %lu bytes of json. %s record",
             yajl_get_bytes_consumed(hand),
             p->record_complete ? "found" : "didnt find");

        *buf_loc += yajl_get_bytes_consumed(hand);
        found = p->record_complete;
    }
    p->record_complete = false;
    yajl_free(hand);
    return found;
}

bool quasar_parse_end(quasar_parse_context *ctx) {
    elog(DEBUG1, "entering function %s", __func__);

    yajl_complete_parse(ctx->handle);
    return ((parser*)ctx->p)->record_complete;
}

void quasar_parse_set_slot(quasar_parse_context *ctx, TupleTableSlot *slot) {
    elog(DEBUG1, "entering function %s", __func__);

    parser *p = (parser *) ctx->p;
    int i;

    for (i = p->table->ncols - 1; i >= 0; --i) {
        if (!p->table->cols[i]->used) {
            slot->tts_isnull[i] = true;
            slot->tts_values[i] = PointerGetDatum(NULL);
        } else {
            slot->tts_isnull[i] = false;
        }
    }

    p->slot = slot;
}
