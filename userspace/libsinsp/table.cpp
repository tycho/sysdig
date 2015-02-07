/*
Copyright (C) 2013-2014 Draios inc.

This file is part of sysdig.

sysdig is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

sysdig is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with sysdig.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#ifndef _WIN32
#include <curses.h>
#endif

#include "sinsp.h"
#include "sinsp_int.h"
#include "../../driver/ppm_ringbuffer.h"
#include "filter.h"
#include "filterchecks.h"
#include "table.h"

extern sinsp_filter_check_list g_filterlist;

//
//
// Table sorter functor
typedef struct table_row_cmp
{
	bool operator()(const sinsp_sample_row& src, const sinsp_sample_row& dst)
	{
		ppm_cmp_operator op;

		if(m_ascending)
		{
			op = CO_LT;
		}
		else
		{
			op = CO_GT;
		}

		return flt_compare(op, m_type, 
			src.m_values[m_colid].m_val, 
			dst.m_values[m_colid].m_val, 
			src.m_values[m_colid].m_len, 
			dst.m_values[m_colid].m_len);
	}

	uint32_t m_colid;
	ppm_param_type m_type;
	bool m_ascending;
}table_row_cmp;

sinsp_table::sinsp_table(sinsp* inspector)
{
	m_inspector = inspector;
	m_is_key_present = false;
	m_is_merge_key_present = false;
	m_fld_pointers = NULL;
	m_premerge_fld_pointers = NULL;
	m_postmerge_fld_pointers = NULL;
	m_n_fields = 0;
	m_n_premerge_fields = 0;
	m_n_postmerge_fields = 0;
	m_refresh_interval = SINSP_TABLE_DEFAULT_REFRESH_INTERVAL_NS;
	m_next_flush_time_ns = 0;
	m_printer = new sinsp_filter_check_reference();
	m_buffer = &m_buffer1;
	m_is_sorting_ascending = false;
	m_sorting_col = 0;
	m_do_merging = true;
	m_types = &m_premerge_types;
	m_table = &m_premerge_table;
}

sinsp_table::~sinsp_table()
{
	uint32_t j;

	for(j = 0; j < m_chks_to_free.size(); j++)
	{
		delete m_chks_to_free[j];
	}

	if(m_premerge_fld_pointers != NULL)
	{
		delete[] m_premerge_fld_pointers;
	}

	if(m_postmerge_fld_pointers != NULL)
	{
		delete[] m_postmerge_fld_pointers;
	}

	delete m_printer;
}

void sinsp_table::configure(const string& fmt, const string& merge_fmt)
{
	uint32_t j;
	string lfmt(fmt);

	if(lfmt == "")
	{
		throw sinsp_exception("empty table initializer");
	}

	//
	// Parse the string and extract the tokens
	//
	const char* cfmt = lfmt.c_str();

	m_extractors.clear();
	uint32_t lfmtlen = (uint32_t)lfmt.length();

	for(j = 0; j < lfmtlen;)
	{
		uint32_t preamble_len = 0;
		bool is_this_the_key = false;
		sinsp_filter_check::aggregation ag = sinsp_filter_check::A_NONE;

		switch(cfmt[j])
		{
			case '*':
				if(m_is_key_present)
				{
					throw sinsp_exception("invalid table configuration");
				}

				m_is_key_present = true;
				is_this_the_key = true;
				preamble_len = 1;
				break;
			case 'S':
				ag = sinsp_filter_check::A_SUM;
				preamble_len = 1;
				break;
			case 'T':
				ag = sinsp_filter_check::A_TIME_AVG;
				preamble_len = 1;
				break;
			case 'm':
				ag = sinsp_filter_check::A_MIN;
				preamble_len = 1;
				break;
			case 'M':
				ag = sinsp_filter_check::A_MAX;
				preamble_len = 1;
				break;
			default:
				break;
		}

		if(j == lfmtlen - 1)
		{
			throw sinsp_exception("invalid table configuration");
		}

		sinsp_filter_check* chk = g_filterlist.new_filter_check_from_fldname(string(cfmt + j + preamble_len), 
			m_inspector,
			false);

		if(chk == NULL)
		{
			throw sinsp_exception("invalid table token " + string(cfmt + j + preamble_len));
		}

		chk->m_aggregation = ag;
		m_chks_to_free.push_back(chk);

		j += chk->parse_field_name(cfmt + j + preamble_len) + preamble_len;
		ASSERT(j <= lfmt.length());

		while(cfmt[j] == ' ' || cfmt[j] == '\t' || cfmt[j] == ',')
		{
			j++;
		}

		if(is_this_the_key)
		{
			m_extractors.insert(m_extractors.begin(), chk);
		}
		else
		{
			m_extractors.push_back(chk);
		}
	}

	m_premerge_fld_pointers = new sinsp_table_field[m_extractors.size()];
	m_fld_pointers = m_premerge_fld_pointers;
	m_n_premerge_fields = (uint32_t)m_extractors.size();
	m_n_fields = m_n_premerge_fields;

	//
	// Make sure this is a valid table
	//
	if(!m_is_key_present)
	{
		throw sinsp_exception("table is missing a key");
	}

	if(m_n_fields < 2)
	{
		throw sinsp_exception("table has no values");
	}

	for(auto it = m_extractors.begin(); it != m_extractors.end(); ++it)
	{
		m_premerge_types.push_back((*it)->get_field_info()->m_type);
		m_premerge_legend.push_back(*(*it)->get_field_info());
	}

	m_premerge_vals_array_sz = (m_n_fields - 1) * sizeof(sinsp_table_field);
	m_vals_array_sz = m_premerge_vals_array_sz;

	//
	// Now generate the real legend, coming after the table merge process 
	//
	if(merge_fmt == "")
	{
		//
		// No merge string. We can stop here
		//
		m_do_merging = false;
		return;
	}

	char* cmfmt = (char*)merge_fmt.c_str();
	uint32_t mfmtlen = (uint32_t)merge_fmt.length();

	for(j = 0; j < mfmtlen;)
	{
		uint32_t preamble_len = 0;
		bool is_this_the_key = false;
		sinsp_filter_check::aggregation ag = sinsp_filter_check::A_NONE;

		switch(cmfmt[j])
		{
			case '*':
				if(m_is_merge_key_present)
				{
					throw sinsp_exception("invalid table configuration");
				}

				m_is_merge_key_present = true;
				is_this_the_key = true;
				preamble_len = 1;
				break;
			case 'S':
				ag = sinsp_filter_check::A_SUM;
				preamble_len = 1;
				break;
			case 'T':
				ag = sinsp_filter_check::A_TIME_AVG;
				preamble_len = 1;
				break;
			case 'm':
				ag = sinsp_filter_check::A_MIN;
				preamble_len = 1;
				break;
			case 'M':
				ag = sinsp_filter_check::A_MAX;
				preamble_len = 1;
				break;
			default:
				break;
		}

		if(j == mfmtlen - 1)
		{
			throw sinsp_exception("invalid table merge configuration");
		}

		char* scnum = cmfmt + j + preamble_len;
		uint32_t cnum; 
		uint32_t ns = sscanf(scnum, "%" PRIu32, &cnum);

		if(ns != 1 || cnum >= m_n_fields)
		{
			throw sinsp_exception("invalid table merge identifier");
		}

		m_merge_columns.push_back(cnum);

		sinsp_filter_check* chk = m_extractors[cnum];

		chk->m_merge_aggregation = ag;

		if(is_this_the_key)
		{
			m_mergers.insert(m_mergers.begin(), chk);
		}
		else
		{
			m_mergers.push_back(chk);
		}

		//
		// Skip spaces
		//
		j += preamble_len + 1;
		while(j < mfmtlen && (cmfmt[j] == ' ' || cmfmt[j] == '\t' || cmfmt[j] == ','))
		{
			j++;
		}
	}

	m_postmerge_fld_pointers = new sinsp_table_field[m_mergers.size()];
	m_n_postmerge_fields = (uint32_t)m_mergers.size();

	if(!m_is_merge_key_present)
	{
		throw sinsp_exception("table is missing the merge key");
	}

	if(m_merge_columns.size() < 2)
	{
		throw sinsp_exception("merged table has no values");
	}

	for(auto it = m_mergers.begin(); it != m_mergers.end(); ++it)
	{
		m_postmerge_types.push_back((*it)->get_field_info()->m_type);
		m_postmerge_legend.push_back(*(*it)->get_field_info());
	}

	m_postmerge_vals_array_sz = (m_n_postmerge_fields - 1) * sizeof(sinsp_table_field);
}

void sinsp_table::add_row(bool merging)
{
	uint32_t j;

	sinsp_table_field key(m_fld_pointers[0].m_val, m_fld_pointers[0].m_len);
	auto it = m_table->find(key);

	if(it == m_table->end())
	{
		//
		// New entry
		//
		key.m_val = m_buffer->copy(key.m_val, key.m_len);
		m_vals = (sinsp_table_field*)m_buffer->reserve(m_vals_array_sz);

		for(j = 1; j < m_n_fields; j++)
		{
			uint32_t vlen = get_field_len(j);
			m_vals[j - 1].m_val = m_buffer->copy(m_fld_pointers[j].m_val, vlen);
			m_vals[j - 1].m_len = vlen;
		}

		(*m_table)[key] = m_vals;
	}
	else
	{
		//
		// Existing entry
		//
		m_vals = it->second;

		for(j = 1; j < m_n_fields; j++)
		{
			if(merging)
			{
				add_fields(j, &m_fld_pointers[j], m_mergers[j]->m_merge_aggregation);
			}
			else
			{
				add_fields(j, &m_fld_pointers[j], m_extractors[j]->m_aggregation);
			}
		}
	}
}

bool sinsp_table::process_event(sinsp_evt* evt)
{
	bool res = false;
	uint32_t j;

	//
	// Extract the values and create the row to add
	//
	for(j = 0; j < m_n_premerge_fields; j++)
	{
		uint32_t len;
		uint8_t* val = m_extractors[j]->extract(evt, &len);

		//
		// XXX For the moment, we drop samples that contain empty values.
		// At a certain point we will want to introduce the concept of zero
		// by default.
		//
		if(val == NULL)
		{
			return res;
		}

		sinsp_table_field* pfld = &(m_premerge_fld_pointers[j]);

		pfld->m_val = val;
		pfld->m_len = get_field_len(j);
	}

	//
	// Add the row
	//
	add_row(false);

	return res;
}

void sinsp_table::flush(sinsp_evt* evt)
{
	if(m_next_flush_time_ns != 0)
	{
		//
		// Time to emit the sample! 
		// If there is a merging step, switch the types to point to the merging ones.
		//
		if(m_do_merging)
		{
			m_types = &m_postmerge_types;
			m_table = &m_merge_table;
			m_n_fields = m_n_postmerge_fields;
			m_vals_array_sz = m_postmerge_vals_array_sz;
			m_fld_pointers = m_postmerge_fld_pointers;
		}

		//
		// Emit the sample
		//
		create_sample();

		//
		// Switch the data storage so that the current one is still usable by the 
		// consumers of the table.
		//
		switch_buffers();

		//
		// Reinitialize the tables
		//
		m_buffer->clear();
		m_premerge_table.clear();
		m_merge_table.clear();
	}

	uint64_t ts = evt->get_ts();
	m_next_flush_time_ns = ts - (ts % m_refresh_interval) + m_refresh_interval;

	return;
}

void sinsp_table::stdout_print()
{
	vector<filtercheck_field_info>* legend = get_legend();

	for(auto it = m_sample_data.begin(); it != m_sample_data.end(); ++it)
	{
		for(uint32_t j = 0; j < m_n_fields - 1; j++)
		{
			m_printer->set_val(m_types->at(j + 1), 
				it->m_values[j].m_val, 
				it->m_values[j].m_len,
				legend->at(j).m_print_format);
				printf("%s ", m_printer->tostring_nice(NULL));
//				printf("%s ", m_printer->tostring(NULL));
		}

			printf("\n");
	}

		printf("----------------------\n");
}

vector<sinsp_sample_row>* sinsp_table::get_sample()
{
	if(m_sample_data.size() != 0)
	{

		if(m_sorting_col >= m_sample_data[0].m_values.size())
		{
			throw sinsp_exception("invalid table sorting column");
		}

		table_row_cmp cc;
		cc.m_colid = m_sorting_col;

		cc.m_ascending = m_is_sorting_ascending;
		cc.m_type = m_types->at(m_sorting_col + 1);

//mvprintw(4, 10, "s%d:%d", (int)m_sorting_col, (int)m_is_sorting_ascending);
//refresh();

		sort(m_sample_data.begin(),
			m_sample_data.end(),
			cc);
	}

#ifdef _WIN32
	stdout_print();
#endif

	//
	// Restore the type list used for event processing
	//
	m_types = &m_premerge_types;
	m_table = &m_premerge_table;
	m_n_fields = m_n_premerge_fields;
	m_vals_array_sz = m_premerge_vals_array_sz;
	m_fld_pointers = m_premerge_fld_pointers;

	return &m_sample_data;
}

void sinsp_table::set_sorting_col(uint32_t col)
{
	uint32_t n_fields;
	vector<ppm_param_type>* types;

	if(m_do_merging)
	{
		n_fields = m_n_postmerge_fields;
		types = &m_postmerge_types;
	}
	else
	{
		n_fields = m_n_premerge_fields;
		types = &m_premerge_types;
	}

	if(col == 0)
	{
		throw sinsp_exception("cannot sort by key");
	}

	if(col > n_fields)
	{
		throw sinsp_exception("invalid table sorting column");
	}

	if(col == m_sorting_col + 1)
	{
		m_is_sorting_ascending = !m_is_sorting_ascending;
	}
	else
	{
		switch(types->at(col))
		{
			case PT_INT8:
			case PT_INT16:
			case PT_INT32:
			case PT_INT64:
			case PT_UINT8:
			case PT_UINT16:
			case PT_UINT32:
			case PT_UINT64:
			case PT_RELTIME:
			case PT_ABSTIME:
				m_is_sorting_ascending = false;
				break;
			default:
				m_is_sorting_ascending = true;
				break;
		}
	}

	m_sorting_col = col - 1;
}

void sinsp_table::create_sample()
{
	uint32_t j;
	m_sample_data.clear();
	sinsp_sample_row row;

	//
	// If merging is on, perform the merge and switch to the merged table 
	//
	if(m_do_merging)
	{
		m_table = &m_merge_table;
		m_merge_table.clear();

		for(auto it = m_premerge_table.begin(); it != m_premerge_table.end(); ++it)
		{
			for(j = 0; j < m_n_postmerge_fields; j++)
			{
				sinsp_table_field* pfld = &(m_postmerge_fld_pointers[j]);

				uint32_t col = m_merge_columns[j];
				if(col == 0)
				{
					pfld->m_val = it->first.m_val;
					pfld->m_len = it->first.m_len;
				}
				else
				{
					pfld->m_val = it->second[col - 1].m_val;
					pfld->m_len = it->second[col - 1].m_len;
				}
			}

			add_row(true);
		}
	}
	else
	{
		m_table = &m_premerge_table;
	}

	//
	// Emit the table
	//
	for(auto it = m_table->begin(); it != m_table->end(); ++it)
	{
		row.m_key = it->first;

		row.m_values.clear();

		sinsp_table_field* fields = it->second;
		for(j = 0; j < m_n_fields - 1; j++)
		{
			row.m_values.push_back(fields[j]);
		}

		m_sample_data.push_back(row);
	}

}

void sinsp_table::add_fields_sum(ppm_param_type type, sinsp_table_field *dst, sinsp_table_field *src)
{
	uint8_t* operand1 = dst->m_val;
	uint8_t* operand2 = src->m_val;

	switch(type)
	{
	case PT_INT8:
		*(int8_t*)operand1 += *(int8_t*)operand2;
		return;
	case PT_INT16:
		*(int16_t*)operand1 += *(int16_t*)operand2;
		return;
	case PT_INT32:
		*(int32_t*)operand1 += *(int32_t*)operand2;
		return;
	case PT_INT64:
		*(int64_t*)operand1 += *(int64_t*)operand2;
		return;
	case PT_UINT8:
		*(uint8_t*)operand1 += *(uint8_t*)operand2;
		return;
	case PT_UINT16:
		*(uint16_t*)operand1 += *(uint16_t*)operand2;
		return;
	case PT_UINT32:
		*(uint32_t*)operand1 += *(uint32_t*)operand2;
		return;
	case PT_UINT64:
	case PT_RELTIME:
	case PT_ABSTIME:
		*(uint64_t*)operand1 += *(uint64_t*)operand2;
		return;
	default:
		return;
	}
}

void sinsp_table::add_fields_max(ppm_param_type type, sinsp_table_field *dst, sinsp_table_field *src)
{
	uint8_t* operand1 = dst->m_val;
	uint8_t* operand2 = src->m_val;

	switch(type)
	{
	case PT_INT8:
		if(*(int8_t*)operand1 < *(int8_t*)operand2)
		{
			*(int8_t*)operand1 = *(int8_t*)operand2;
		}
		return;
	case PT_INT16:
		if(*(int16_t*)operand1 < *(int16_t*)operand2)
		{
			*(int16_t*)operand1 = *(int16_t*)operand2;
		}
		return;
	case PT_INT32:
		if(*(int32_t*)operand1 < *(int32_t*)operand2)
		{
			*(int32_t*)operand1 = *(int32_t*)operand2;
		}
		return;
	case PT_INT64:
		if(*(int64_t*)operand1 < *(int64_t*)operand2)
		{
			*(int64_t*)operand1 = *(int64_t*)operand2;
		}
		return;
	case PT_UINT8:
		if(*(uint8_t*)operand1 < *(uint8_t*)operand2)
		{
			*(uint8_t*)operand1 = *(uint8_t*)operand2;
		}
		return;
	case PT_UINT16:
		if(*(uint16_t*)operand1 < *(uint16_t*)operand2)
		{
			*(uint16_t*)operand1 = *(uint16_t*)operand2;
		}
		return;
	case PT_UINT32:
		if(*(uint32_t*)operand1 < *(uint32_t*)operand2)
		{
			*(uint32_t*)operand1 = *(uint32_t*)operand2;
		}
		return;
	case PT_UINT64:
	case PT_RELTIME:
	case PT_ABSTIME:
		if(*(uint64_t*)operand1 < *(uint64_t*)operand2)
		{
			*(uint64_t*)operand1 = *(uint64_t*)operand2;
		}
		return;
	default:
		return;
	}
}

void sinsp_table::add_fields(uint32_t dst_id, sinsp_table_field* src, uint32_t aggr)
{
	ppm_param_type type = (*m_types)[dst_id];
	sinsp_table_field* dst = &(m_vals[dst_id - 1]);

	switch(aggr)
	{
	case sinsp_filter_check::A_NONE:
		return;
	case sinsp_filter_check::A_SUM:
		add_fields_sum(type, dst, src);		
		return;
	case sinsp_filter_check::A_MAX:
		add_fields_max(type, dst, src);		
		return;
	default:
		ASSERT(false);
		return;
	}
}

uint32_t sinsp_table::get_field_len(uint32_t id)
{
	ppm_param_type type;
	sinsp_table_field *fld;

	type = (*m_types)[id];
	fld = &(m_fld_pointers[id]);

	switch(type)
	{
	case PT_INT8:
		return 1;
	case PT_INT16:
		return 2;
	case PT_INT32:
		return 4;
	case PT_INT64:
	case PT_FD:
	case PT_PID:
	case PT_ERRNO:
		return 8;
	case PT_FLAGS8:
	case PT_UINT8:
	case PT_SIGTYPE:
		return 1;
	case PT_FLAGS16:
	case PT_UINT16:
	case PT_PORT:
	case PT_SYSCALLID:
		return 2;
	case PT_UINT32:
	case PT_FLAGS32:
	case PT_BOOL:
	case PT_IPV4ADDR:
		return 4;
	case PT_UINT64:
	case PT_RELTIME:
	case PT_ABSTIME:
		return 8;
	case PT_CHARBUF:
		return (uint32_t)(strlen((char*)fld->m_val) + 1);
	case PT_BYTEBUF:
		return fld->m_len;
	case PT_SOCKADDR:
	case PT_SOCKTUPLE:
	case PT_FDLIST:
	case PT_FSPATH:
	default:
		ASSERT(false);
		return false;
	}
}

void sinsp_table::switch_buffers()
{
	if(m_buffer == &m_buffer1)
	{
		m_buffer = &m_buffer2;
	}
	else
	{
		m_buffer = &m_buffer1;
	}
}

sinsp_table_field* sinsp_table::get_row_key(uint32_t rownum)
{
	if(rownum >= m_sample_data.size())
	{
		return NULL;
	}

	return &m_sample_data[rownum].m_key;
}

int32_t sinsp_table::get_row_from_key(sinsp_table_field* key)
{
	uint32_t j;

	for(j = 0; j < m_sample_data.size(); j++)
	{
		sinsp_table_field* rowkey = &(m_sample_data[j].m_key);

		if(*rowkey == *key)
		{
			return j;
		}
	}

	return -1;
}