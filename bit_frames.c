//
// Author: Wolfgang Spraul
//
// This is free and unencumbered software released into the public domain.
// For details see the UNLICENSE file at the root of the source tree.
//

#include "model.h"
#include "bit.h"
#include "parts.h"
#include "control.h"

#define HCLK_BYTES 2

static uint8_t* get_first_minor(struct fpga_bits* bits, int row, int major)
{
	int i, num_frames;

	num_frames = 0;
	for (i = 0; i < major; i++)
		num_frames += get_major_minors(XC6SLX9, i);
	return &bits->d[(row*FRAMES_PER_ROW + num_frames)*FRAME_SIZE];
}

static int get_bit(struct fpga_bits* bits,
	int row, int major, int minor, int bit_i)
{
	return frame_get_bit(get_first_minor(bits, row, major)
		+ minor*FRAME_SIZE, bit_i);
}

static void set_bit(struct fpga_bits* bits,
	int row, int major, int minor, int bit_i)
{
	return frame_set_bit(get_first_minor(bits, row, major)
		+ minor*FRAME_SIZE, bit_i);
}

static void clear_bit(struct fpga_bits* bits,
	int row, int major, int minor, int bit_i)
{
	return frame_clear_bit(get_first_minor(bits, row, major)
		+ minor*FRAME_SIZE, bit_i);
}

struct bit_pos
{
	int row;
	int major;
	int minor;
	int bit_i;
};

static int get_bitp(struct fpga_bits* bits, struct bit_pos* pos)
{
	return get_bit(bits, pos->row, pos->major, pos->minor, pos->bit_i);
}

static void set_bitp(struct fpga_bits* bits, struct bit_pos* pos)
{
	set_bit(bits, pos->row, pos->major, pos->minor, pos->bit_i);
}

static void clear_bitp(struct fpga_bits* bits, struct bit_pos* pos)
{
	clear_bit(bits, pos->row, pos->major, pos->minor, pos->bit_i);
}

static struct bit_pos s_default_bits[] = {
	{ 0, 0, 3, 66 },
	{ 0, 1, 23, 1034 },
	{ 0, 1, 23, 1035 },
	{ 0, 1, 23, 1039 },
	{ 2, 0, 3, 66 }};

// swpos_mip is relative to a tile, i.e. major (x) and
// row/v64_i (y) are defined outside.
struct swpos_mip
{
	int mip; // 0-18, even only
	int two_bits_o; // 0-127, even only
	int two_bits_val; // 0-3
	int one_bit_o; // 0-127, 1-6 bits up or down from two-bit val
	swidx_t uni_dir;
	swidx_t rev_dir; // NO_SWITCH for unidirectional switches
};

struct swpos_yx
{
	int y;
	int x;
	swidx_t idx;
};

#define MAX_YX_SWITCHES 1024

struct extract_state
{
	struct fpga_model* model;
	struct fpga_bits* bits;
	int num_mips;
	struct swpos_mip mip[MAX_SWITCHBOX_SIZE];
	// yx switches are fully extracted ones pointing into the
	// model, stored here for later processing into nets.
	int num_sw_yx;
	struct swpos_yx sw_yx[MAX_YX_SWITCHES]; // needs to be dynamically alloced...
};

static int extract_iobs(struct fpga_model* model, struct fpga_bits* bits)
{
	int i, num_iobs, iob_y, iob_x, iob_idx, dev_idx, rc;
	uint32_t* u32_p;
	const char* iob_sitename;
	struct fpga_device* dev;

	num_iobs = get_num_iobs(XC6SLX9);
	for (i = 0; i < num_iobs; i++) {
		u32_p = (uint32_t*) &bits->d[IOB_DATA_START + i*IOB_ENTRY_LEN];
		if (!u32_p[0] && !u32_p[1])
			continue;
		iob_sitename = get_iob_sitename(XC6SLX9, i);
		if (!iob_sitename) {
			HERE();
			continue;
		}
		rc = fpga_find_iob(model, iob_sitename, &iob_y, &iob_x, &iob_idx);
		if (rc) FAIL(rc);
		dev_idx = fpga_dev_idx(model, iob_y, iob_x, DEV_IOB, iob_idx);
		if (dev_idx == NO_DEV) FAIL(EINVAL);
		dev = FPGA_DEV(model, iob_y, iob_x, dev_idx);

		// we only support 2 hardcoded types of IOB right now
		// todo: bit 7 goes on when out-net connected?
		if ((u32_p[0] & 0xFFFFFF7F) == 0x00000100
		    && u32_p[1] == 0x06001100) {
			dev->instantiated = 1;
			strcpy(dev->u.iob.ostandard, IO_LVCMOS33);
			dev->u.iob.drive_strength = 12;
			dev->u.iob.O_used = 1;
			dev->u.iob.slew = SLEW_SLOW;
			dev->u.iob.suspend = SUSP_3STATE;
			u32_p[0] = 0;
			u32_p[1] = 0;
		} else if (u32_p[0] == 0x00000107
			   && u32_p[1] == 0x0B002400) {
			dev->instantiated = 1;
			strcpy(dev->u.iob.istandard, IO_LVCMOS33);
			dev->u.iob.bypass_mux = BYPASS_MUX_I;
			dev->u.iob.I_mux = IMUX_I;
			u32_p[0] = 0;
			u32_p[1] = 0;
		} else HERE();
	}
	return 0;
fail:
	return rc;
}

static int extract_logic(struct fpga_model* model, struct fpga_bits* bits)
{
	int dev_idx, row, row_pos, rc;
	int x, y, byte_off;
	uint8_t* u8_p;
	uint64_t u64;
	const char* lut_str;

	for (x = LEFT_SIDE_WIDTH; x < model->x_width-RIGHT_SIDE_WIDTH; x++) {
		if (!is_atx(X_FABRIC_LOGIC_COL|X_CENTER_LOGIC_COL, model, x))
			continue;
		for (y = TOP_IO_TILES; y < model->y_height - BOT_IO_TILES; y++) {
			if (!has_device_type(model, y, x, DEV_LOGIC, LOGIC_M))
				continue;
			row = which_row(y, model);
			row_pos = pos_in_row(y, model);
			if (row == -1 || row_pos == -1 || row_pos == 8) {
				HERE();
				continue;
			}
			if (row_pos > 8) row_pos--;
			u8_p = get_first_minor(bits, row, model->x_major[x]);
			byte_off = row_pos * 8;
			if (row_pos >= 8) byte_off += HCLK_BYTES;

			// M device
			dev_idx = fpga_dev_idx(model, y, x, DEV_LOGIC, DEV_LOGM);
			if (dev_idx == NO_DEV) FAIL(EINVAL);

			// A6_LUT
			if (frame_get_u32(u8_p + 24*FRAME_SIZE + byte_off + 4)
			    || frame_get_u32(u8_p + 25*FRAME_SIZE + byte_off + 4)) {
				u64 = read_lut64(u8_p + 24*FRAME_SIZE, (byte_off+4)*8);
				{ int logic_base[6] = {0,1,0,0,1,0};
				  lut_str = lut2bool(u64, 64, &logic_base, /*flip_b0*/ 1); }
				if (*lut_str) {
					rc = fdev_logic_set_lut(model, y, x, DEV_LOGM,
						A6_LUT, lut_str, ZTERM);
					if (rc) FAIL(rc);
					*(uint32_t*)(u8_p+24*FRAME_SIZE+byte_off+4) = 0;
					*(uint32_t*)(u8_p+25*FRAME_SIZE+byte_off+4) = 0;
				}
			}
			// B6_LUT
			if (frame_get_u32(u8_p + 21*FRAME_SIZE + byte_off + 4)
			    || frame_get_u32(u8_p + 22*FRAME_SIZE + byte_off + 4)) {
				u64 = read_lut64(u8_p + 21*FRAME_SIZE, (byte_off+4)*8);
				{ int logic_base[6] = {1,1,0,1,0,1};
				  lut_str = lut2bool(u64, 64, &logic_base, /*flip_b0*/ 1); }
				if (*lut_str) {
					rc = fdev_logic_set_lut(model, y, x, DEV_LOGM,
						B6_LUT, lut_str, ZTERM);
					if (rc) FAIL(rc);
					*(uint32_t*)(u8_p+21*FRAME_SIZE+byte_off+4) = 0;
					*(uint32_t*)(u8_p+22*FRAME_SIZE+byte_off+4) = 0;
				}
			}
			// C6_LUT
			if (frame_get_u32(u8_p + 24*FRAME_SIZE + byte_off)
			    || frame_get_u32(u8_p + 25*FRAME_SIZE + byte_off)) {
				u64 = read_lut64(u8_p + 24*FRAME_SIZE, byte_off*8);
				{ int logic_base[6] = {0,1,0,0,1,0};
				  lut_str = lut2bool(u64, 64, &logic_base, /*flip_b0*/ 1); }
				if (*lut_str) {
					rc = fdev_logic_set_lut(model, y, x, DEV_LOGM,
						C6_LUT, lut_str, ZTERM);
					if (rc) FAIL(rc);
					*(uint32_t*)(u8_p+24*FRAME_SIZE+byte_off) = 0;
					*(uint32_t*)(u8_p+25*FRAME_SIZE+byte_off) = 0;
				}
			}
			// D6_LUT
			if (frame_get_u32(u8_p + 21*FRAME_SIZE + byte_off)
			    || frame_get_u32(u8_p + 22*FRAME_SIZE + byte_off)) {
				u64 = read_lut64(u8_p + 21*FRAME_SIZE, byte_off*8);
				{ int logic_base[6] = {1,1,0,1,0,1};
				  lut_str = lut2bool(u64, 64, &logic_base, /*flip_b0*/ 1); }
				if (*lut_str) {
					rc = fdev_logic_set_lut(model, y, x, DEV_LOGM,
						D6_LUT, lut_str, ZTERM);
					if (rc) FAIL(rc);
					*(uint32_t*)(u8_p+21*FRAME_SIZE+byte_off) = 0;
					*(uint32_t*)(u8_p+22*FRAME_SIZE+byte_off) = 0;
				}
			}

			// X device
			u64 = frame_get_u64(u8_p + 26*FRAME_SIZE + byte_off);
			if ( u64 ) {
				// 21, 22, 36 and 37 are actually not default
				// and can go off with the FFMUXes or routing
				// say D over the FF to DQ etc. (AFFMUX=b37,
				// BFFMUX=b36, CFFMUX=b22, DFFMUX=b21).
				if (!(u64 & (1ULL<<1) && u64 & (1ULL<<2)
				      && u64 & (1ULL<<7) && u64 & (1ULL<<21)
				      && u64 & (1ULL<<22) && u64 & (1ULL<<36)
				      && u64 & (1ULL<<37) && u64 & (1ULL<<39))) {
					HERE();
					continue;
				}
				if (u64 & ~(0x000000B000600086ULL)) {
					HERE();
					continue;
				}

				dev_idx = fpga_dev_idx(model, y, x, DEV_LOGIC, DEV_LOGX);
				if (dev_idx == NO_DEV) FAIL(EINVAL);
				*(uint64_t*)(u8_p+26*FRAME_SIZE+byte_off) = 0;

				// A6_LUT
				u64 = read_lut64(u8_p + 27*FRAME_SIZE, (byte_off+4)*8);
				{ int logic_base[6] = {1,1,0,1,1,0};
				  lut_str = lut2bool(u64, 64, &logic_base, /*flip_b0*/ 0); }
				if (*lut_str) {
					rc = fdev_logic_set_lut(model, y, x, DEV_LOGX,
						A6_LUT, lut_str, ZTERM);
					if (rc) FAIL(rc);
					*(uint32_t*)(u8_p+27*FRAME_SIZE+byte_off+4) = 0;
					*(uint32_t*)(u8_p+28*FRAME_SIZE+byte_off+4) = 0;
				}
				// B6_LUT
				u64 = read_lut64(u8_p + 29*FRAME_SIZE, (byte_off+4)*8);
				{ int logic_base[6] = {1,1,0,1,1,0};
				  lut_str = lut2bool(u64, 64, &logic_base, /*flip_b0*/ 0); }
				if (*lut_str) {
					rc = fdev_logic_set_lut(model, y, x, DEV_LOGX,
						B6_LUT, lut_str, ZTERM);
					*(uint32_t*)(u8_p+29*FRAME_SIZE+byte_off+4) = 0;
					*(uint32_t*)(u8_p+30*FRAME_SIZE+byte_off+4) = 0;
				}
				// C6_LUT
				u64 = read_lut64(u8_p + 27*FRAME_SIZE, byte_off*8);
				{ int logic_base[6] = {0,1,0,0,0,1};
				  lut_str = lut2bool(u64, 64, &logic_base, /*flip_b0*/ 0); }
				if (*lut_str) {
					rc = fdev_logic_set_lut(model, y, x, DEV_LOGX,
						C6_LUT, lut_str, ZTERM);
					*(uint32_t*)(u8_p+27*FRAME_SIZE+byte_off) = 0;
					*(uint32_t*)(u8_p+28*FRAME_SIZE+byte_off) = 0;
				}
				// D6_LUT
				u64 = read_lut64(u8_p + 29*FRAME_SIZE, byte_off*8);
				{ int logic_base[6] = {0,1,0,0,0,1};
				  lut_str = lut2bool(u64, 64, &logic_base, /*flip_b0*/ 0); }
				if (*lut_str) {
					rc = fdev_logic_set_lut(model, y, x, DEV_LOGX,
						D6_LUT, lut_str, ZTERM);
					*(uint32_t*)(u8_p+29*FRAME_SIZE+byte_off) = 0;
					*(uint32_t*)(u8_p+30*FRAME_SIZE+byte_off) = 0;
				}
			}
		}
	}
	return 0;
fail:
	return rc;
}

static int mip_is_set(struct extract_state* es, int y, int x,
	struct swpos_mip* swpos, int* is_set)
{
	int row_num, row_pos, start_in_frame, two_bits_val, rc;

	*is_set = 0;
	is_in_row(es->model, y, &row_num, &row_pos);
	if (row_num == -1 || row_pos == -1
	    || row_pos == HCLK_POS) FAIL(EINVAL);
	if (row_pos > HCLK_POS)
		start_in_frame = (row_pos-1)*64 + 16;
	else
		start_in_frame = row_pos*64;

	two_bits_val = 
		(get_bit(es->bits, row_num, es->model->x_major[x], swpos->mip,
			start_in_frame + swpos->two_bits_o/2) << 1)
		| (get_bit(es->bits, row_num, es->model->x_major[x], swpos->mip+1,
		    start_in_frame + swpos->two_bits_o/2) << 2);
	if (two_bits_val != swpos->two_bits_val)
		return 0;

	if (!get_bit(es->bits, row_num, es->model->x_major[x],
		swpos->mip + (swpos->one_bit_o&1),
		start_in_frame + swpos->one_bit_o/2))
		return 0;
	*is_set = 1;
	return 0;
fail:
	return rc;
}

static int mip_clear_bits(struct extract_state* es, int y, int x,
	struct swpos_mip* swpos)
{
	int row_num, row_pos, start_in_frame, rc;

	is_in_row(es->model, y, &row_num, &row_pos);
	if (row_num == -1 || row_pos == -1
	    || row_pos == HCLK_POS) FAIL(EINVAL);
	if (row_pos > HCLK_POS)
		start_in_frame = (row_pos-1)*64 + 16;
	else
		start_in_frame = row_pos*64;

	clear_bit(es->bits, row_num, es->model->x_major[x], swpos->mip,
		start_in_frame + swpos->two_bits_o/2);
	clear_bit(es->bits, row_num, es->model->x_major[x], swpos->mip + 1,
		start_in_frame + swpos->two_bits_o/2);
	clear_bit(es->bits, row_num, es->model->x_major[x], swpos->mip + (swpos->one_bit_o&1),
		start_in_frame + swpos->one_bit_o/2);

	return 0;
fail:
	return rc;
}

static int extract_routing_switches(struct extract_state* es, int y, int x)
{
	struct fpga_tile* tile;
	int i, is_set, rc;

	tile = YX_TILE(es->model, y, x);
if (y != 68 || x != 12) return 0;

	// build tile-relative model of all routing switches and
	// store in the extract state
	if (!es->num_mips) {
	}

	for (i = 0; i < es->num_mips; i++) {
		rc = mip_is_set(es, y, x, &es->mip[i], &is_set);
		if (rc) FAIL(rc);
		if (!is_set) continue;

		if (tile->switches[es->mip[i].uni_dir] & SWITCH_BIDIRECTIONAL)
			HERE();
		if (tile->switches[es->mip[i].uni_dir] & SWITCH_USED)
			HERE();
		if (es->num_sw_yx >= MAX_YX_SWITCHES)
			{ FAIL(ENOTSUP); }
		es->sw_yx[es->num_sw_yx].y = y;
		es->sw_yx[es->num_sw_yx].x = x;
		es->sw_yx[es->num_sw_yx].idx = es->mip[i].uni_dir;
		es->num_sw_yx++;
		rc = mip_clear_bits(es, y, x, &es->mip[i]);
		if (rc) FAIL(rc);
	}
	return 0;
fail:
	return rc;
}

static int extract_switches(struct extract_state* es)
{
	int x, y, rc;

	// go through all tiles, look for one with switches
	// go through each switch, lookup device, is_enabled() -> enable
	for (x = 0; x < es->model->x_width; x++) {
		for (y = 0; y < es->model->y_height; y++) {
			if (is_atx(X_ROUTING_COL, es->model, x)
			    && y >= TOP_IO_TILES
			    && y < es->model->y_height-BOT_IO_TILES
			    && !is_aty(Y_ROW_HORIZ_AXSYMM|Y_CHIP_HORIZ_REGS,
					es->model, y)) {
				rc = extract_routing_switches(es, y, x);
				if (rc) FAIL(rc);
			}
#if 0
// when all switches are supported, we can turn this
// on to make the model more robust
			if (tile->num_switches)
				fprintf(stderr, "%i unsupported switches in y%02i x%02i\n",
					tile->num_switches, y, x);
#endif
		}
	}
	return 0;
fail:
	return rc;
}

// these are roughly ordered in rows and columns as they are wired
// up in the routing switchbox.

#define DIRBEG_ROW 12
static const int dirbeg_matrix[] =
{
	W_WW4, W_NW4, W_NN4, W_NE4, W_NW2, W_NN2, W_NE2, W_EE2, W_WR1, W_NL1, W_EL1, W_NR1,
	W_SS4, W_SW4, W_EE4, W_SE4, W_WW2, W_SW2, W_SS2, W_SE2, W_SR1, W_WL1, W_SL1, W_ER1
};

static const int dirbeg_matrix_topnum[] =
{
	3, 3, 3, 3, 3, 3, 3, 3, /*WR1*/ 0, /*NL1*/ 2, /*EL1*/ 2, /*NR1*/ 3,
	3, 3, 3, 3, 3, 3, 3, 3, /*SR1*/ 0, /*WL1*/ 2, /*SL1*/ 3, /*ER1*/ 0
};

#define LOGOUT_ROW 6
static const int logicout_matrix[] =
{
	M_BMUX, M_DQ, M_D, X_BMUX, X_DQ, X_D,
	M_AMUX, M_CQ, M_C, X_AMUX, X_CQ, X_C,
	M_DMUX, M_BQ, M_B, X_DMUX, X_BQ, X_B,
	M_CMUX, M_AQ, M_A, X_CMUX, X_AQ, X_A
};

#define LOGIN_ROW 8
static const int logicin_matrix[] =
{
		  /*mip 12*/   /*mip 14*/   /*mip 16*/   /*mip 18*/
	/* 000 */ M_C6, X_D6,  X_C1, X_DX,  M_B3, X_A3,  X_B2, FAN_B,
	/* 016 */ M_B1, M_DI,  M_A3, X_B3,  M_C2, M_DX,  M_D6, X_C6,
	/* 032 */ M_C5, X_D5,  M_CX, M_D2,  M_B4, X_A4,  M_A1, M_CE,
	/* 048 */ M_CI, X_A2,  M_A4, X_B4,  X_CX, X_D1,  M_D5, X_C5,
	/* 064 */ M_C4, X_D4,  M_D1, X_BX,  M_B5, X_A5,  M_A2, M_BI,
	/* 080 */ X_A1, X_CE,  M_A5, X_B5,  M_BX, X_D2,  M_D4, X_C4,
	/* 096 */ M_C3, X_D3,  M_AX, X_C2,  M_B6, X_A6,  M_AI, X_B1,
	/* 112 */ M_B2, M_WE,  M_A6, X_B6,  M_C1, X_AX,  M_D3, X_C3
};

static int mod4_calc(int a, int b)
{
	return (unsigned int) (a+b)%4;
}

static int construct_extract_state(struct extract_state* es, struct fpga_model* model)
{
	char from_str[MAX_WIRENAME_LEN], to_str[MAX_WIRENAME_LEN];
	int i, j, k, l, cur_mip, cur_two_bits_o, cur_two_bits_val, rc;
	int logicin_i;

	memset(es, 0, sizeof(*es));
	es->model = model;
	if (model->first_routing_y == -1)
		FAIL(EINVAL);

	// switches from logicout to dirwires (6*2*2*4*6=576 switches)
	for (i = 0; i < DIRBEG_ROW; i++) {
		cur_mip = (i/2)*2;
		for (j = 0; j <= 3; j++) { // 4 wires for each dirwire
			for (k = 0; k <= 1; k++) { // two dirbeg rows
				cur_two_bits_o = j*32 + k*16;
				if (i%2) cur_two_bits_o += 14;
				cur_two_bits_val = ((i%2)^k) ? 1 : 2;
				for (l = 0; l < LOGOUT_ROW; l++) {
					es->mip[es->num_mips].mip = cur_mip;
					es->mip[es->num_mips].two_bits_o = cur_two_bits_o;
					es->mip[es->num_mips].two_bits_val = cur_two_bits_val;

					es->mip[es->num_mips].one_bit_o = j*32+k*16+2+l*2;
					if (!((i%2)^k)) // right side (second minor)
						es->mip[es->num_mips].one_bit_o++;

					snprintf(from_str, sizeof(from_str), "LOGICOUT%i", logicout_matrix[j*LOGOUT_ROW + (k?5-l:l)]);
					snprintf(to_str, sizeof(to_str), "%sB%i",
						wire_base(dirbeg_matrix[k*DIRBEG_ROW+i]), mod4_calc(dirbeg_matrix_topnum[k*DIRBEG_ROW+i], -j));
					es->mip[es->num_mips].uni_dir = fpga_switch_lookup(es->model,
						es->model->first_routing_y, es->model->first_routing_x,
						strarray_find(&es->model->str, from_str),
						strarray_find(&es->model->str, to_str));
					if (es->mip[es->num_mips].uni_dir == NO_SWITCH) {
						fprintf(stderr, "#E routing switch %s -> %s not in model\n",
							from_str, to_str);
						FAIL(EINVAL);
					}

					es->mip[es->num_mips].rev_dir = NO_SWITCH;
					es->num_mips++;
				}
			}
		}
	}

	// VCC (32 switches) and GFAN (32 switches +4 bidir)
	for (i = 12; i <= 18; i+=2) { // mip12/14/16/18
		for (j = 0; j <= 3; j++) { // 4 rows
			for (k = 0; k <= 1; k++) { // two switch destinations

				// VCC
				es->mip[es->num_mips].mip = i;
				es->mip[es->num_mips].two_bits_o = 32*j + (k?14:0);
				es->mip[es->num_mips].two_bits_val = 3;
				es->mip[es->num_mips].one_bit_o = 32*j+2;
				logicin_i = j*2*LOGIN_ROW + i-12 + k;

				if (i == 14 || i == 18) {
					es->mip[es->num_mips].two_bits_o += 16;
					es->mip[es->num_mips].one_bit_o += 16;
					es->mip[es->num_mips].one_bit_o += !k;
					logicin_i += LOGIN_ROW;
				} else
					es->mip[es->num_mips].one_bit_o += k;
				
				snprintf(to_str, sizeof(to_str), "LOGICIN_B%i", logicin_matrix[logicin_i]);
				es->mip[es->num_mips].uni_dir = fpga_switch_lookup(es->model,
					es->model->first_routing_y, es->model->first_routing_x,
					strarray_find(&es->model->str, "VCC_WIRE"),
					strarray_find(&es->model->str, to_str));
				if (es->mip[es->num_mips].uni_dir == NO_SWITCH) {
					fprintf(stderr, "#E routing switch VCC_WIRE -> %s not in model\n",
						to_str);
					FAIL(EINVAL);
				}
				es->mip[es->num_mips].rev_dir = NO_SWITCH;
				es->mip[es->num_mips+1] = es->mip[es->num_mips];
				es->num_mips++;

				// GFAN
				if (i == 14 || i == 18) {
					es->mip[es->num_mips].two_bits_o -= 16;
					es->mip[es->num_mips].one_bit_o -= 16;
					logicin_i -= LOGIN_ROW;
				} else { // 12 or 16
					es->mip[es->num_mips].two_bits_o += 16;
					es->mip[es->num_mips].one_bit_o += 16;
					logicin_i += LOGIN_ROW;
				}
				snprintf(from_str, sizeof(from_str), "GFAN%i", j<2?1:0);
				if (logicin_matrix[logicin_i] == FAN_B)
					strcpy(to_str, "FAN_B");
				else
					snprintf(to_str, sizeof(to_str), "LOGICIN_B%i", logicin_matrix[logicin_i]);
				es->mip[es->num_mips].uni_dir = fpga_switch_lookup(es->model,
					es->model->first_routing_y, es->model->first_routing_x,
					strarray_find(&es->model->str, from_str),
					strarray_find(&es->model->str, to_str));
				if (es->mip[es->num_mips].uni_dir == NO_SWITCH) {
					fprintf(stderr, "#E routing switch %s -> %s not in model\n",
						from_str, to_str);
					FAIL(EINVAL);
				}
				// two bidir switches from and to GFAN0 (6 and 35),
				// two from and to GFAN1 (51 and 53)
				if (logicin_matrix[logicin_i] == 6
				    || logicin_matrix[logicin_i] == 35
				    || logicin_matrix[logicin_i] == 51
				    || logicin_matrix[logicin_i] == 53) {
					es->mip[es->num_mips].rev_dir = fpga_switch_lookup(es->model,
						es->model->first_routing_y, es->model->first_routing_x,
						strarray_find(&es->model->str, to_str),
						strarray_find(&es->model->str, from_str));
					if (es->mip[es->num_mips].rev_dir == NO_SWITCH) {
						fprintf(stderr, "#E rev routing switch %s -> %s not in model\n",
							to_str, from_str);
						FAIL(EINVAL);
					}
				} else
					es->mip[es->num_mips].rev_dir = NO_SWITCH;
				es->num_mips++;
			}
		}
	}
	return 0;
fail:
	return rc;
}

int extract_model(struct fpga_model* model, struct fpga_bits* bits)
{
	struct extract_state es;
	net_idx_t net_idx;
	int i, rc;

	rc = construct_extract_state(&es, model);
	if (rc) FAIL(rc);
	es.bits = bits;
	for (i = 0; i < sizeof(s_default_bits)/sizeof(s_default_bits[0]); i++) {
		if (!get_bitp(bits, &s_default_bits[i]))
			FAIL(EINVAL);
		clear_bitp(bits, &s_default_bits[i]);
	}

	rc = extract_iobs(model, bits);
	if (rc) FAIL(rc);
	rc = extract_logic(model, bits);
	if (rc) FAIL(rc);
	rc = extract_switches(&es);
	if (rc) FAIL(rc);

	// turn switches into nets
	if (model->nets)
		HERE(); // should be empty here
	for (i = 0; i < es.num_sw_yx; i++) {
		rc = fpga_net_new(model, &net_idx);
		if (rc) FAIL(rc);
		rc = fpga_net_add_switch(model, net_idx, es.sw_yx[i].y, es.sw_yx[i].x, es.sw_yx[i].idx);
		if (rc) FAIL(rc);
	}
	return 0;
fail:
	return rc;
}

int printf_routing_mips(struct fpga_model* model)
{
	struct extract_state es;
	char bit_str[129];
	int i, j, rc;

	rc = construct_extract_state(&es, model);
	if (rc) FAIL(rc);
	bit_str[128] = 0;
	for (i = 0; i < es.num_mips; i++) {
		for (j = 0; j < 128; j++)
			bit_str[j] = '0';
		if (es.mip[i].two_bits_val & 2)
			bit_str[es.mip[i].two_bits_o] = '1';
		if (es.mip[i].two_bits_val & 1)
			bit_str[es.mip[i].two_bits_o+1] = '1';
		bit_str[es.mip[i].one_bit_o] = '1';
		printf("mip%02i %s %s %s %s\n", es.mip[i].mip,
			fpga_switch_str(model, model->first_routing_y, model->first_routing_x, es.mip[i].uni_dir, SW_TO),
			bit_str,
			fpga_switch_str(model, model->first_routing_y, model->first_routing_x, es.mip[i].uni_dir, SW_FROM),
			es.mip[i].rev_dir != NO_SWITCH ? "<->" : "->");
	}
	return 0;
fail:
	return rc;
}

int write_model(struct fpga_bits* bits, struct fpga_model* model)
{
	int i;
	for (i = 0; i < sizeof(s_default_bits)/sizeof(s_default_bits[0]); i++)
		set_bitp(bits, &s_default_bits[i]);
	return 0;
}
