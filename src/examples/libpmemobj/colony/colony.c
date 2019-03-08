/*
 * Copyright 2019, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <libpmemobj.h>

POBJ_LAYOUT_BEGIN(colony);
POBJ_LAYOUT_TOID(colony, struct colony);
POBJ_LAYOUT_END(colony);

POBJ_LAYOUT_BEGIN(block);
POBJ_LAYOUT_TOID(block, struct block);
POBJ_LAYOUT_TOID(block, TOID(struct block));
POBJ_LAYOUT_END(block);

POBJ_LAYOUT_BEGIN(free_block);
POBJ_LAYOUT_TOID(free_block, struct free_block);
POBJ_LAYOUT_END(free_block);

POBJ_LAYOUT_BEGIN(free_idx);
POBJ_LAYOUT_TOID(free_idx, struct free_idx);
POBJ_LAYOUT_END(free_idx);

POBJ_LAYOUT_BEGIN(table);
POBJ_LAYOUT_TOID(table, int);
POBJ_LAYOUT_TOID(table, PMEMoid);
POBJ_LAYOUT_END(table);

enum array_types {
	UNKNOWN_ARRAY_TYPE,
	INT_ARRAY_TYPE,
	PMEMOID_ARRAY_TYPE,
	MAX_ARRAY_TYPE
};

struct block {
	/* indicates the beginning of table */
	PMEMoid table;

	/* number of occupied elements */
	size_t block_size;
	/* number of block in colony */
	size_t block_nr;

	/* index of last added element (idx in block) */
	size_t idx_last;
	/* number of free (removed) elements */
	size_t free_elem;

	/* pointer on the previous block */
	TOID(struct block) prev;
	/* pointer on the next block */
	TOID(struct block) next;
};

struct free_idx {
	/* index (in colony) of free element */
	size_t idx_free;
	/* pointer on the previous free element */
	TOID(struct free_idx) prev;
	/* pointer on the next free element */
	TOID(struct free_idx) next;
};

struct free_block {
	/* pointer on the free block */
	TOID(struct block) block_free;
	/* pointer on the previous free block */
	TOID(struct free_block) prev;
	/* pointer on the next free block */
	TOID(struct free_block) next;
};

struct colony {
	/* type of elements in colony */
	enum array_types element_type;

	/* pointer on the first block */
	TOID(struct block) block_head;
	/* pointer on the last block */
	TOID(struct block) block_tail;
	/* number of elements in each block */
	size_t block_capacity;
	/* number of blocks in colony */
	size_t block_count;
	/* number of occupied elements */
	size_t colony_size;
	/* total capacity = block_capacity * block_count */
	size_t colony_capacity;

	/* number of free elements (holes) */
	size_t free_idx_count;
	/* pointer on the last free element */
	TOID(struct free_idx) free_idx_tail;

	/* number of free blocks */
	size_t free_block_count;
	/* pointer on the last free block */
	TOID(struct free_block) free_block_tail;
};

/*
 * get type -- parses argument given as type of colony
 */
static enum array_types
get_type(const char *type_name)
{
	const char *names[MAX_ARRAY_TYPE] = {"", "int", "PMEMoid"};
	enum array_types type;

	for (type = (enum array_types)(MAX_ARRAY_TYPE - 1);
			type > UNKNOWN_ARRAY_TYPE;
			type = (enum array_types)(type - 1)) {
		if (strcmp(names[type], type_name) == 0)
			break;
	}
	if (type == UNKNOWN_ARRAY_TYPE)
		fprintf(stderr, "unknown type: %s\n", type_name);

	return type;
}

/*
 * file_exists -- checks if the file exists
 */
static int
file_exists(const char *filename)
{
	if (access(filename, F_OK) != -1)
		return 1;
	else
		return 0;
}

/*
 * capacity_get -- returns the capacity of colony
 */
static size_t
capacity_get(TOID(struct colony) c)
{
	return D_RO(c)->colony_capacity;
}

/*
 * size_get -- returns the number of occupied addresses
 */
static size_t
size_get(TOID(struct colony) c)
{
	return D_RO(c)->colony_size;
}

/*
 * block_get_by_idx -- returns the pointer to the block in which element with
 * given index resides
 */
static TOID(struct block)
block_get_by_idx(TOID(struct colony) c, size_t colony_idx)
{
	size_t block_nr = (size_t)(colony_idx / D_RO(c)->block_capacity);
	/* sets the pointer to the block */
	TOID(struct block) block_with_idx = D_RO(c)->block_head;
	for (size_t i = 0; i < block_nr; i++) {
		block_with_idx = D_RO(block_with_idx)->next;
	}

	return block_with_idx;
}

/*
 * block_get_by_nr -- returns the pointer to the block with given block_nr
 */
static TOID(struct block)
block_get_by_nr(TOID(struct colony) c, size_t block_nr)
{
	TOID(struct block) block_with_nr = D_RO(c)->block_head;
	for (size_t i = 0; i < block_nr; i++) {
		block_with_nr = D_RO(block_with_nr)->next;
	}

	return block_with_nr;
}

/*
 * int_insert_at_idx -- inserts the integer element in the colony at colony_idx
 */
static void
int_insert_at_idx(PMEMobjpool *pop, TOID(struct colony) c, size_t colony_idx,
								int element)
{
	assert(pmemobj_tx_stage() == TX_STAGE_WORK);
	printf("el: %d", element);
	/* sets the pointer to the table in which is a free element */
	TOID(struct block) block_with_free = block_get_by_idx(c, colony_idx);

	size_t block_idx = colony_idx % D_RO(c)->block_capacity;
	TOID(int) tmp;
	TOID_ASSIGN(tmp, D_RW(block_with_free)->table);
	D_RW(tmp)[block_idx] = element;
	D_RW(block_with_free)->block_size++;
}

/*
 * pmemoid_insert_at_idx -- inserts the pimemoid element in the colony at
 * colony_idx
 */
static void
pmemoid_insert_at_idx(PMEMobjpool *pop, TOID(struct colony) c,
					size_t colony_idx, PMEMoid element)
{
	assert(pmemobj_tx_stage() == TX_STAGE_WORK);
	/* sets the pointer to the table in which is a free element */
	TOID(struct block) block_with_free = block_get_by_idx(c, colony_idx);
	TOID(PMEMoid) table;
	TOID_ASSIGN(table, D_RW(block_with_free)->table);

	size_t block_idx = colony_idx % D_RO(c)->block_capacity;

	D_RW(table)[block_idx] = element;
	D_RW(block_with_free)->block_size++;
}

/*
 * insert_at_idx -- inserts the integer element in the colony at colony_idx
 */
static void
insert_at_idx(PMEMobjpool *pop, TOID(struct colony) c, size_t colony_idx,
								void *element)
{
	assert(pmemobj_tx_stage() == TX_STAGE_WORK);
	if (D_RO(c)->element_type == INT_ARRAY_TYPE)
		int_insert_at_idx(pop, c, colony_idx, *(int *)element);
	else if (D_RO(c)->element_type == PMEMOID_ARRAY_TYPE)
		pmemoid_insert_at_idx(pop, c, colony_idx, *(PMEMoid *)element);
}

/*
 * table_int_create -- allocates the table of integers in the block
 */
static void
table_int_create(PMEMobjpool *pop, TOID(struct colony) c, TOID(struct block) b)
{
	size_t s = sizeof(int) * (D_RO(c)->block_capacity);

	TX_BEGIN(pop) {
		TX_ADD_FIELD(b, table);
		D_RW(b)->table = pmemobj_tx_alloc(s, TOID_TYPE_NUM(int));
	} TX_ONABORT {
		fprintf(stderr, "%s: transaction aborted: %s\n", __func__,
			pmemobj_errormsg());
		abort();
	} TX_END
}

/*
 * table_pmemoid_create -- allocates the table of PMEMoids in the block
 */
static void
table_pmemoid_create(PMEMobjpool *pop, TOID(struct colony) c,
							TOID(struct block) b)
{
	size_t s = sizeof(PMEMoid) * (D_RO(c)->block_capacity);
	TOID(PMEMoid) table;
	TOID_ASSIGN(table, D_RW(b)->table);

	TX_BEGIN(pop) {
		TX_ADD_FIELD(b, table);
		table = TX_ALLOC(PMEMoid, s);
	} TX_ONABORT {
		fprintf(stderr, "%s: transaction aborted: %s\n", __func__,
			pmemobj_errormsg());
		abort();
	} TX_END
}

/*
 * table_int_delete -- frees the table of integers
 */
static void
table_int_delete(PMEMobjpool *pop, PMEMoid table_del)
{
	TOID(int) table;
	TOID_ASSIGN(table, table_del);

	TX_BEGIN(pop) {
		TX_FREE(table);
	} TX_ONABORT {
		fprintf(stderr, "%s: transaction aborted: %s\n", __func__,
			pmemobj_errormsg());
		abort();
	} TX_END
}

/*
 * table_pmemoid_delete -- frees the table of PMEMoids
 */
static void
table_pmemoid_delete(PMEMobjpool *pop, PMEMoid table_del)
{
	TOID(PMEMoid) table;
	TOID_ASSIGN(table, table_del);

	TX_BEGIN(pop) {
		TX_FREE(table);
	} TX_ONABORT {
		fprintf(stderr, "%s: transaction aborted: %s\n", __func__,
			pmemobj_errormsg());
		abort();
	} TX_END
}

/*
 * block_init_int -- allocates a block, creates a table, assigns values
 */
static void
block_init(PMEMobjpool *pop, TOID(struct colony) c, TOID(struct block) * b)
{
	assert(pmemobj_tx_stage() == TX_STAGE_WORK);
	*b = TX_ZNEW(struct block);

	if (D_RW(c)->element_type == INT_ARRAY_TYPE)
		table_int_create(pop, c, *b);
	else if (D_RO(c)->element_type == PMEMOID_ARRAY_TYPE)
		table_pmemoid_create(pop, c, *b);

	D_RW(*b)->block_size = 0;
	D_RW(*b)->idx_last = -1;
	D_RW(*b)->free_elem = 0;

	D_RW(*b)->block_nr = D_RO(c)->block_count;
}



/*
 * block_constructor -- constructor of the one block
 */
static int
block_constructor(PMEMobjpool *pop, TOID(struct colony) c)
{
	assert(pmemobj_tx_stage() == TX_STAGE_WORK);
	/* if it is a first block */
	if (TOID_IS_NULL(D_RO(c)->block_head)) {

		block_init(pop, c, &D_RW(c)->block_tail);

		D_RW(D_RW(c)->block_tail)->prev = TOID_NULL(struct block);
		D_RW(D_RW(c)->block_tail)->next = TOID_NULL(struct block);
		D_RW(c)->block_head = D_RW(c)->block_tail;		
	} else {
		assert(TOID_IS_NULL(D_RW(D_RW(c)->block_tail)->next));
		block_init(pop, c, &D_RW(D_RW(c)->block_tail)->next);

		D_RW(D_RW(D_RW(c)->block_tail)->next)->prev = D_RO(c)->
								block_tail;
		D_RW(c)->block_tail = D_RO(D_RO(c)->block_tail)->next;
		D_RW(D_RW(c)->block_tail)->next = TOID_NULL(struct block);
	}
	D_RW(c)->colony_capacity += D_RO(c)->block_capacity;
	D_RW(c)->block_count++;

	return 0;
}

/*
 * free_idxs_delete -- destructor of the list with free indexes
 */
static int
free_idxes_delete(PMEMobjpool *pop, TOID(struct colony) c)
{
	TOID(struct free_idx) idx_del = D_RO(c)->free_idx_tail;

	TX_BEGIN(pop) {
		TX_ADD_FIELD(c, free_idx_count);
		TX_ADD_FIELD(c, free_idx_tail);

		while (!TOID_IS_NULL(D_RO(idx_del)->prev)) {

			idx_del = D_RO(idx_del)->prev;

			TX_FREE(D_RW(idx_del)->next);
		}
		TX_FREE(idx_del);

		D_RW(c)->free_idx_count = 0;
		D_RW(c)->free_idx_tail = TOID_NULL(struct free_idx);
	} TX_ONABORT {
		fprintf(stderr, "%s: transaction aborted: %s\n", __func__,
			pmemobj_errormsg());
		abort();
	} TX_END

	return 0;
}

/*
 * free_idxes_update_ -- updates indexes in list of free_idx after block removal
 */
static void
free_idxes_update(TOID(struct colony) c, size_t deleted_block_nr)
{
	assert(pmemobj_tx_stage() == TX_STAGE_WORK);
	size_t idx_above_which = (deleted_block_nr - 1) * D_RO(c)->
								block_capacity;
	TOID(struct free_idx) free_idx_to_update = D_RO(c)->free_idx_tail;

	for (size_t i = 0; i < D_RO(c)->free_idx_count; i++) {
		if (D_RW(free_idx_to_update)->idx_free > idx_above_which)
			D_RW(free_idx_to_update)->idx_free -= D_RO(c)->
								block_capacity;
		free_idx_to_update = D_RO(free_idx_to_update)->prev;
	}
}

/*
 * blocks_nr_update_ -- updates block_nrs after block (the one before
 * block_to_update) removal
 */
static void
blocks_nr_update(TOID(struct colony) c, TOID(struct block) block_to_update)
{
	assert(pmemobj_tx_stage() == TX_STAGE_WORK);
	for (size_t i = D_RO(block_to_update)->block_nr; i < D_RO(c)->
							block_count; i++) {
		free_idxes_update(c, D_RO(block_to_update)->block_nr - 1);
		D_RW(block_to_update)->block_nr--;
		block_to_update = D_RO(block_to_update)->next;
	}
}

/*
 * free_blocks_delete -- destructor of the free blocks
 */
static int
free_blocks_delete(PMEMobjpool *pop, TOID(struct colony) c)
{
	TOID(struct free_block) block_del = D_RO(c)->free_block_tail;

	TX_BEGIN(pop) {
		TX_ADD_FIELD(c, block_count);
		TX_ADD_FIELD(c, colony_capacity);
		TX_ADD_FIELD(c, free_block_tail);
		TX_ADD_FIELD(c, free_block_count);

		while (!TOID_IS_NULL(D_RO(block_del)->prev)) {

			D_RW(D_RW(D_RW(block_del)->block_free)->prev)->next =
					D_RO(D_RO(block_del)->block_free)->next;
			D_RW(D_RW(D_RW(block_del)->block_free)->next)->prev =
					D_RO(D_RO(block_del)->block_free)->prev;

			if (D_RO(c)->element_type == INT_ARRAY_TYPE)
				table_int_delete(pop, D_RW(D_RW(block_del)->
							block_free)->table);
			else if (D_RO(c)->element_type == PMEMOID_ARRAY_TYPE)
				table_pmemoid_delete(pop, D_RW(D_RW(block_del)->
							block_free)->table);

			blocks_nr_update(c, D_RW(D_RW(block_del)->block_free)->
									next);

			block_del = D_RW(block_del)->prev;

			TX_FREE(D_RW(D_RW(block_del)->next)->block_free);
		}
		TX_FREE(D_RW(block_del)->block_free);

		D_RW(c)->block_count -= D_RO(c)->free_block_count;
		D_RW(c)->colony_capacity -= D_RO(c)->block_capacity * D_RO(c)->
							free_block_count;

		D_RW(c)->free_block_tail = TOID_NULL(struct free_block);
		D_RW(c)->free_block_count = 0;
	} TX_ONABORT {
		fprintf(stderr, "%s: transaction aborted: %s\n", __func__,
			pmemobj_errormsg());
		abort();
	} TX_END

	return 0;
}

/*
 * blocks_delete -- destructor of the blocks
 */
static int
blocks_delete(PMEMobjpool *pop, TOID(struct colony) c)
{
	TOID(struct block) block_del = D_RO(c)->block_head;

	TX_BEGIN(pop) {
		TX_ADD_FIELD(c, block_head);
		TX_ADD_FIELD(c, block_tail);
		TX_ADD_FIELD(c, block_count);

		while (!TOID_IS_NULL(D_RO(block_del)->next)) {

			table_int_delete(pop, D_RW(block_del)->table);

			block_del = D_RW(block_del)->next;

			TX_FREE(D_RW(block_del)->prev);
		}
		TX_FREE(block_del);

		D_RW(c)->block_head = TOID_NULL(struct block);
		D_RW(c)->block_tail = TOID_NULL(struct block);
		free_idxes_delete(pop, c);
		free_blocks_delete(pop, c);
		D_RW(c)->block_count = 0;
	} TX_ONABORT {
		fprintf(stderr, "%s: transaction aborted: %s\n", __func__,
			pmemobj_errormsg());
		abort();
	} TX_END

	return 0;
}

/*
 * free_idx_get -- returns first unoccupied (!= removed) index in colony
 */
static size_t
free_idx_get(TOID(struct colony) c)
{
	size_t block_idx = D_RO(D_RO(c)->block_tail)->idx_last + 1;
	size_t colony_idx = (D_RO(c)->block_count - 1) * D_RO(c)->block_capacity
								+ block_idx;

	return colony_idx;
}

/*
 * free_block_add_to -- adds the free block (all elements removed) to the list
 * with free blocks
 */
static int
free_block_add_to(PMEMobjpool *pop, TOID(struct colony) c, size_t block_nr)
{
	assert(pmemobj_tx_stage() == TX_STAGE_WORK);
	/* sets the pointer on the block */
	TOID(struct block) block_with_free = block_get_by_nr(c, block_nr);

	D_RW(D_RW(D_RW(c)->free_block_tail)->next)->block_free =
								block_with_free;
	D_RW(D_RW(D_RW(c)->free_block_tail)->next)->prev = D_RO(c)->
								free_block_tail;
	D_RW(D_RW(D_RW(c)->free_block_tail)->next)->next =
						TOID_NULL(struct free_block);

	D_RW(c)->free_block_tail = D_RO(D_RO(c)->free_block_tail)->next;
	D_RW(D_RW(D_RW(c)->free_block_tail)->prev)->next = D_RO(c)->
								free_block_tail;

	D_RW(c)->free_idx_count -= D_RO(c)->block_capacity;
	D_RW(c)->free_block_count++;

	return 0;
}

/*
 * free_idx_add_to -- saves the address where the removal was made
 */
static int
free_idx_add_to(PMEMobjpool *pop, TOID(struct colony) c, size_t idx)
{
	assert(pmemobj_tx_stage() == TX_STAGE_WORK);
	/* inserts the address into the list of free addresses */
	D_RW(D_RW(D_RW(c)->free_idx_tail)->next)->idx_free = idx;
	D_RW(D_RW(D_RW(c)->free_idx_tail)->next)->prev = D_RO(c)->free_idx_tail;
	D_RW(D_RW(D_RW(c)->free_idx_tail)->next)->next =
						TOID_NULL(struct free_idx);

	D_RW(c)->free_idx_tail = D_RO(D_RO(c)->free_idx_tail)->next;
	D_RW(D_RW(D_RW(c)->free_idx_tail)->prev)->next = D_RO(c)->free_idx_tail;

	D_RW(c)->free_idx_count++;

	/* sets the pointer on the block */
	TOID(struct block) block_with_free = block_get_by_idx(c, idx);

	D_RW(block_with_free)->free_elem++;
	/* if all elements are removed */
	if (D_RO(block_with_free)->free_elem == D_RO(c)->block_capacity) {
		size_t block_nr = idx / D_RO(c)->block_capacity;
		free_block_add_to(pop, c, block_nr);
	}

	return 0;
}

/*
 * free_block_take_from -- takes the address from the list of free blocks to
 * insertion
 */
static size_t
free_block_take_from(PMEMobjpool *pop, TOID(struct colony) c)
{
	assert(pmemobj_tx_stage() == TX_STAGE_WORK);
	/* takes the first address from the last block from the list */
	size_t idx = (D_RO(D_RO(D_RO(c)->free_block_tail)->block_free)->
					block_nr - 1) * D_RO(c)->block_capacity;

	/* if it was the last one then null */
	D_RW(c)->free_block_tail = D_RW(D_RW(c)->free_block_tail)->prev;
	D_RW(D_RW(c)->free_block_tail)->next = TOID_NULL(struct free_block);

	/* function free_idx_add_to in loop below updates this value */
	D_RW(D_RW(D_RW(c)->free_block_tail)->block_free)->free_elem = 0;

	D_RW(c)->free_block_count--;

	/* adds the rest of elements to the list of free addresses */
	for (size_t i = 1; i < D_RO(c)->block_capacity; i++) {
		free_idx_add_to(pop, c, idx + i);
	}

	return idx;
}

/*
 * free_idx_take_from -- takes the address from the list of free addresses to
 * insertion
 */
static size_t
free_idx_take_from(PMEMobjpool *pop, TOID(struct colony) c)
{
	assert(pmemobj_tx_stage() == TX_STAGE_WORK);
	/* takes the last address from the list of free addresses */
	size_t idx = D_RO(D_RO(c)->free_idx_tail)->idx_free;

	/* if it was the last one then null */
	D_RW(c)->free_idx_tail = D_RW(D_RW(c)->free_idx_tail)->prev;
	D_RW(D_RW(c)->free_idx_tail)->next = TOID_NULL(struct free_idx);

	/* sets the pointer on the block */
	TOID(struct block) block_with_free = block_get_by_idx(c, idx);
	D_RW(block_with_free)->free_elem--;
	D_RW(c)->free_idx_count--;

	return idx;
}

/*
 * colony_create -- creates empty colony
 */
static int
colony_create(PMEMobjpool *pop, TOID(struct colony) c, enum array_types type,
							int block_capacity)
{
	if (sizeof(type) * block_capacity > PMEMOBJ_MAX_ALLOC_SIZE) {
		fprintf(stderr, "alloc failed: %s\n", pmemobj_errormsg());
		return -1;
	}

	TX_BEGIN(pop) {
		TX_ADD(c);

		D_RW(c)->element_type = type;
		D_RW(c)->block_capacity = block_capacity;
		D_RW(c)->block_count = 0;
		D_RW(c)->colony_capacity = 0;
		D_RW(c)->colony_size = 0;
		D_RW(c)->free_idx_count = 0;
		D_RW(c)->free_block_count = 0;

		D_RW(c)->block_head = TOID_NULL(struct block);
		D_RW(c)->block_tail = TOID_NULL(struct block);
		D_RW(c)->free_idx_tail = TOID_NULL(struct free_idx);
		D_RW(c)->free_block_tail = TOID_NULL(struct free_block);
	} TX_ONABORT {
		fprintf(stderr, "%s: transaction aborted: %s\n", __func__,
			pmemobj_errormsg());
		abort();
	} TX_END

	return 0;
}

/*
 * colony_delete -- deletes colony
 */
static int
colony_delete(PMEMobjpool *pop, TOID(struct colony) c)
{
	TX_BEGIN(pop) {
		TX_ADD(c);

		blocks_delete(pop, c);
	} TX_ONABORT {
		fprintf(stderr, "%s: transaction aborted: %s\n", __func__,
			pmemobj_errormsg());
		abort();
	} TX_END

	return 0;
}

/*
 * insert_int -- inserts the integer element into the colony
 */
static int
insert_element(PMEMobjpool *pop, TOID(struct colony) c, void *element)
{
	size_t colony_idx = 0;

	TX_BEGIN(pop) {
		TX_ADD(c);

		/* if is a vacant in the colony */
		if (size_get(c) != capacity_get(c)) {

			/* if is a free address (hole) */
			if (D_RO(c)->free_idx_count) {
				colony_idx = free_idx_take_from(pop, c);
				insert_at_idx(pop, c, colony_idx, element);

			/* if is a free block (block of holes) */
			} else if (D_RO(c)->free_block_count) {
				colony_idx = free_block_take_from(pop, c);
				insert_at_idx(pop, c, colony_idx, element);

			/* if is an unoccupied address in the newest block */
			} else {
				colony_idx = free_idx_get(c);
				insert_at_idx(pop, c, colony_idx, element);
				D_RW(D_RW(c)->block_tail)->idx_last++;
		}

		/* creates a new block */
		} else {
			block_constructor(pop, c);
			colony_idx = 0;//D_RO(c)->colony_size + 1;
			insert_at_idx(pop, c, colony_idx, element);
			D_RW(D_RW(c)->block_tail)->idx_last++;
		}

		D_RW(c)->colony_size++;

	} TX_ONABORT {
		fprintf(stderr, "%s: transaction aborted: %s\n", __func__,
							pmemobj_errormsg());
		abort();
	} TX_END

	return colony_idx;
}

/*
 * remove_int -- removes the integer element from the colony
 */
static int
remove_element(PMEMobjpool *pop, TOID(struct colony) c, size_t colony_idx)
{
	if (colony_idx >= D_RO(c)->colony_capacity) {
		printf("element does not exist\n");
		return -1;
	}

	/* sets the pointer on the block */
	TOID(struct block) block_with_elem = block_get_by_idx(c, colony_idx);

	TX_BEGIN(pop) {
		TX_ADD_FIELD(c, colony_size);

		D_RW(c)->colony_size--;
		D_RW(block_with_elem)->block_size--;

		free_idx_add_to(pop, c, colony_idx);
	} TX_ONABORT {
		fprintf(stderr, "%s: transaction aborted: %s\n", __func__,
							pmemobj_errormsg());
		abort();
	} TX_END

	return 0;
}

int
main(int argc, char *argv[])
{
	PMEMobjpool *pop = NULL;
	const char *path = argv[1];

	if (!file_exists(path)) {
		if ((pop = pmemobj_create(path, POBJ_LAYOUT_NAME(colony),
					PMEMOBJ_MIN_POOL, 0666)) == NULL) {
			printf("failed to create pool\n");
			return 1;
		}
	} else {
		if ((pop = pmemobj_open(path, POBJ_LAYOUT_NAME(colony)))
								== NULL) {
			printf("failed to open pool\n");
			return 1;
		}
	}

	enum array_types element_type = get_type(argv[2]);
	int capacity = atoll(argv[3]);

	TOID(struct colony) col;
	POBJ_NEW(pop, &col, struct colony, NULL, NULL);
	
	int creation = colony_create(pop, col, element_type, capacity);

	if (creation == 0)
		printf("create col\n");
	else printf("not create col\n");

	int i = 4;
	
	insert_element(pop, col, &i);

	remove_element(pop, col, 1);

	free_blocks_delete(pop, col);

	if (colony_delete(pop, col) == 0)
		printf("del col\n");
	else printf("not del col\n");

	pmemobj_close(pop);

	return 0;
}

// realloc changes capacity

// insert_range remove_range
