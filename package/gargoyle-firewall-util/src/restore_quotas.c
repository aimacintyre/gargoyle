/*  restore_quotas --	Used to initialize and restore bandwidth quotas based on UCI config files 
 *  			and any previously saved quota data in /usr/data/quotas
 *  			Originally designed for use with Gargoyle router firmware (gargoyle-router.com)
 *
 *
 *  Copyright © 2009-2010 by Eric Bishop <eric@gargoyle-router.com>
 *
 *  This file is free software: you may copy, redistribute and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation, either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  This file is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <erics_tools.h>
#include <uci.h>
#include <nft_bwctl.h>
#define malloc safe_malloc
#define strdup safe_strdup

#define INGRESS_INDEX  0
#define EGRESS_INDEX   1
#define COMBINED_INDEX 2

void restore_backup_for_id(char* id, char* quota_backup_dir, unsigned char is_individual_other, list* defined_ip_groups);
uint32_t* ip_to_host_int(char* ip_str, int* family);
uint32_t* ip_range_to_host_ints(char* ip_str, int* family);
list* filter_group_from_list(list** orig_ip_list, char* ip_group_str);
int get_ipstr_family(char* ip_str);
char* invert_bitmask(const char* input);

void delete_chain_from_table(char* family, char* table, char* delete_chain);
void run_shell_command(char* command, int free_command_str);
void free_split_pieces(char** split_pieces);

list* get_all_sections_of_type(struct uci_context *ctx, char* package, char* section_type);
char* get_uci_option(struct uci_context* ctx,char* package_name, char* section_name, char* option_name);
char* get_option_value_string(struct uci_option* uopt);

int dry_run;

int main(int argc, char** argv)
{
	char* wan_if = NULL;
	char* local_subnet = NULL;
	char* local_subnet6 = NULL;
	char* death_mark = NULL;
	char* death_mask = NULL;
	char* inverted_death_mask = NULL;
	char* crontab_line = NULL;
    int ret;
	
	unsigned char full_qos_active = 0;
	dry_run = 0;
	char c;
	while((ret = getopt(argc, argv, "W:w:s:S:t:T:d:D:m:M:c:C:qQpP")) != -1) //section, page, css includes, javascript includes, title, output interface variables
	{
        c = ret;
		switch(c)
		{
			case 'W':
			case 'w':
				wan_if = strdup(optarg);
				break;
			case 'S':
			case 's':
				local_subnet = strdup(optarg);
				break;
			case 'T':
			case 't':
				local_subnet6 = strdup(optarg);
				break;
			case 'D':
			case 'd':
				death_mark = strdup(optarg);
				break;
			case 'M':
			case 'm':
				death_mask = strdup(optarg);
				inverted_death_mask = invert_bitmask(death_mask);
				break;
			case 'C':
			case 'c':
				crontab_line = strdup(optarg);
				break;
			case 'Q':
			case 'q':
				full_qos_active = 1;
				break;
			case 'P':
			case 'p':
				dry_run = 1;
				break;
		}
	}

	/* even if parameters are wrong, whack old rules */
	char quota_family[] = "inet";
	char quota_table[] = "fw4";
	char quota_chain_prefix[] = "mangle_";
	char crontab_dir[] = "/etc/crontabs/";
	char crontab_file_path[] = "/etc/crontabs/root";
	char* quota_family_table = dynamic_strcat(3,quota_family," ",quota_table);
	delete_chain_from_table(quota_family, quota_table, "mangle_egress_quotas");
	delete_chain_from_table(quota_family, quota_table, "mangle_ingress_quotas");
	delete_chain_from_table(quota_family, quota_table, "mangle_combined_quotas");
	delete_chain_from_table(quota_family, quota_table, "mangle_forward_quotas");
	delete_chain_from_table(quota_family, quota_table, "nat_quota_redirects");

	if(wan_if == NULL)
	{
		fprintf(stderr, "ERRROR: No wan interface specified\n");
		return 0;
	}
	if(local_subnet == NULL)
	{
		fprintf(stderr, "ERRROR: No local subnet specified\n");
		return 0;
	}
	if(local_subnet6 == NULL)
	{
		fprintf(stderr, "ERRROR: No local subnet6 specified\n");
		return 0;
	}
	if(death_mark == NULL)
	{
		fprintf(stderr, "ERRROR: No death mark specified\n");
		return 0;
	}
	if(death_mask == NULL)
	{
		fprintf(stderr, "ERRROR: No death mask specified\n");
		return 0;
	}

	struct uci_context *ctx = uci_alloc_context();
	struct uci_ptr ptr;

	list* quota_sections = get_all_sections_of_type(ctx, "firewall", "quota");
	if(quota_sections->length > 0)
	{
		/* load defined base ids */
		string_map* defined_base_ids   = initialize_string_map(0);
		string_map* upload_qos_marks   = initialize_string_map(0);
		string_map* download_qos_marks = initialize_string_map(0);

		long_map* up_speeds = initialize_long_map();
		long_map* down_speeds = initialize_long_map();
		list* quota_section_buf = initialize_list();
		while(quota_sections->length > 0)
		{
			char* next_quota = shift_list(quota_sections);
			char* base_id = get_uci_option(ctx, "firewall", next_quota, "id");
			char* exceeded_up_speed_str = get_uci_option(ctx, "firewall", next_quota, "exceeded_up_speed");
			char* exceeded_down_speed_str = get_uci_option(ctx, "firewall", next_quota, "exceeded_down_speed");

			if(base_id != NULL)
			{
				//D, for dummy place holder
				char* oldval = set_string_map_element(defined_base_ids, base_id, strdup("D") );
				if(oldval != NULL) { free(oldval); }
			}
			push_list(quota_section_buf, next_quota);

			if(exceeded_up_speed_str != NULL && exceeded_down_speed_str != NULL)
			{
				long up;
				long down;
				if(sscanf(exceeded_up_speed_str, "%ld", &up) > 0 && sscanf(exceeded_down_speed_str, "%ld", &down) > 0 )
				{
					if(up > 0 && down > 0)
					{
						char* oldval = set_long_map_element(up_speeds, up, strdup(exceeded_up_speed_str) );
						if(oldval != NULL) { free(oldval); }
						oldval = set_long_map_element(down_speeds, down, strdup(exceeded_down_speed_str) );
						if(oldval != NULL) { free(oldval); }
					}
				}
			}
			free(base_id);
			free(exceeded_up_speed_str);
			free(exceeded_down_speed_str);
		}
		unsigned long num_destroyed;
		destroy_list(quota_sections, DESTROY_MODE_FREE_VALUES, &num_destroyed);
		quota_sections = quota_section_buf;

		/* initialize qos mark maps */
		unsigned long mark_band = 2;
		int upload_shift = 0;
		int download_shift = 8;
		while(up_speeds->num_elements > 0)
		{
			unsigned long mark = mark_band << upload_shift;
			char mark_str[10];
			unsigned long smallest_speed;
			char* next_up_speed = remove_smallest_long_map_element(up_speeds, &smallest_speed);
			sprintf(mark_str, "%ld", mark);
			set_string_map_element(upload_qos_marks, next_up_speed, strdup(mark_str));
			free(next_up_speed);
			mark_band++;
		}
		mark_band = 2;
		while(down_speeds->num_elements > 0)
		{
			unsigned long mark = mark_band << download_shift;
			char mark_str[10];
			unsigned long smallest_speed;
			char* next_down_speed = remove_smallest_long_map_element(down_speeds, &smallest_speed);
			sprintf(mark_str, "%ld", mark);
			set_string_map_element(download_qos_marks, next_down_speed, strdup(mark_str));
			free(next_down_speed);
			mark_band++;
		}

		/* initialize chains */
		run_shell_command(dynamic_strcat(5, "nft add chain ", quota_family_table, " ", quota_chain_prefix, "forward_quotas 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(5, "nft add chain ", quota_family_table, " ", quota_chain_prefix, "egress_quotas 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(5, "nft add chain ", quota_family_table, " ", quota_chain_prefix, "ingress_quotas 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(5, "nft add chain ", quota_family_table, " ", quota_chain_prefix, "combined_quotas 2>/dev/null"), 1);

		run_shell_command(dynamic_strcat(3, "nft add chain ", quota_family_table, " nat_quota_redirects 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(3, "nft add rule ", quota_family_table, " nat_quota_redirects ct mark set ct mark \\& 0x00FFFFFF \\| 0x0 2>/dev/null"), 1); // Yes the | 0x0 is not needed, but keeping the pattern consistent
		run_shell_command(dynamic_strcat(3, "nft insert rule ", quota_family_table, " dstnat_lan jump nat_quota_redirects 2>/dev/null"), 1);

		char* no_death_mark_test = dynamic_strcat(3, " ct mark \\& ", death_mask, " == 0x0 ");
		run_shell_command(dynamic_strcat(8, "nft insert rule ", quota_family_table, " input iifname ", wan_if, no_death_mark_test, " jump ", quota_chain_prefix, "combined_quotas  2>/dev/null"), 1); // position 2
		run_shell_command(dynamic_strcat(8, "nft insert rule ", quota_family_table, " input iifname ", wan_if, no_death_mark_test, " jump ", quota_chain_prefix, "ingress_quotas  2>/dev/null"), 1); // position 1
		run_shell_command(dynamic_strcat(8, "nft insert rule ", quota_family_table, " output oifname ", wan_if, no_death_mark_test, " jump ", quota_chain_prefix, "combined_quotas  2>/dev/null"), 1); // position 2
		run_shell_command(dynamic_strcat(8, "nft insert rule ", quota_family_table, " output oifname ", wan_if, no_death_mark_test, " jump ", quota_chain_prefix, "egress_quotas  2>/dev/null"), 1); // position 1

		run_shell_command(dynamic_strcat(5, "nft insert rule ", quota_family_table, " forward jump ", quota_chain_prefix, "forward_quotas 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(10, "nft add rule ", quota_family_table, " ", quota_chain_prefix, "forward_quotas oifname ", wan_if, no_death_mark_test, " jump ", quota_chain_prefix, "egress_quotas 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(10, "nft add rule ", quota_family_table, " ", quota_chain_prefix, "forward_quotas iifname ", wan_if, no_death_mark_test, " jump ", quota_chain_prefix, "ingress_quotas 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(8, "nft add rule ", quota_family_table, " ", quota_chain_prefix, "forward_quotas iifname ", wan_if, no_death_mark_test, " ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0F000000 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(8, "nft add rule ", quota_family_table, " ", quota_chain_prefix, "forward_quotas oifname ", wan_if, no_death_mark_test, " ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0F000000 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(7, "nft add rule ", quota_family_table, " ", quota_chain_prefix, "forward_quotas ct mark \\& 0x0F000000 == 0x0F000000 jump ", quota_chain_prefix, "combined_quotas 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(5, "nft add rule ", quota_family_table, " ", quota_chain_prefix, "forward_quotas ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0 2>/dev/null"), 1);
		free(no_death_mark_test);

		run_shell_command(dynamic_strcat(7, "nft add rule ", quota_family_table, " ", quota_chain_prefix, "egress_quotas ct mark set ct mark \\& ", inverted_death_mask, " \\| 0x0 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(7, "nft add rule ", quota_family_table, " ", quota_chain_prefix, "ingress_quotas ct mark set ct mark \\& ", inverted_death_mask, " \\| 0x0 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(7, "nft add rule ", quota_family_table, " ", quota_chain_prefix, "combined_quotas ct mark set ct mark \\& ", inverted_death_mask, " \\| 0x0 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(5, "nft add rule ", quota_family_table, " ", quota_chain_prefix, "egress_quotas ct mark set ct mark \\& 0x00FFFFFF \\| 0x0 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(5, "nft add rule ", quota_family_table, " ", quota_chain_prefix, "ingress_quotas ct mark set ct mark \\& 0x00FFFFFF \\| 0x0 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(5, "nft add rule ", quota_family_table, " ", quota_chain_prefix, "combined_quotas ct mark set ct mark \\& 0x00FFFFFF \\| 0x0 2>/dev/null"), 1);

		/* add rules */
		char* set_death_mark = dynamic_strcat(5, " ct mark set ct mark \\& ", inverted_death_mask, " \\| ", death_mark, " ");
		list* other_quota_section_names = initialize_list();
		list* defined_ip_groups = initialize_list();

		unlock_bandwidth_semaphore_on_exit();
		while(quota_sections->length > 0 || other_quota_section_names->length > 0)
		{
			char* next_quota = NULL;
			int process_other_quota = 0;
			if(quota_sections->length > 0)
			{
				next_quota = shift_list(quota_sections);
			}
			else
			{
				process_other_quota = 1;
				next_quota = shift_list(other_quota_section_names);
			}

			char* quota_enabled_var  = get_uci_option(ctx, "firewall", next_quota, "enabled");
			int enabled = 1;
			if(quota_enabled_var != NULL)
			{
				if(strcmp(quota_enabled_var, "0") == 0)
				{
					enabled = 0;
				}
			}
			free(quota_enabled_var);
			
			if(enabled)
			{
				char* ip = get_uci_option(ctx, "firewall", next_quota, "ip");
				char* exceeded_up_speed_str = NULL;
				char* exceeded_down_speed_str = NULL;
				if(!full_qos_active) /* without defined up/down speeds we always set hard cutoff, which is what we want when full qos is active */
				{
					exceeded_up_speed_str = get_uci_option(ctx, "firewall", next_quota, "exceeded_up_speed");
					exceeded_down_speed_str = get_uci_option(ctx, "firewall", next_quota, "exceeded_down_speed");
				}
				if(exceeded_up_speed_str == NULL) { exceeded_up_speed_str = strdup(" "); }
				if(exceeded_down_speed_str == NULL) { exceeded_down_speed_str = strdup(" "); }


				if(ip == NULL) { ip = strdup("ALL"); }
				if(strlen(ip) == 0) { ip  = strdup("ALL"); }

				/* remove spaces in ip range definitions */
				while(strstr(ip, " -") != NULL)
				{
					char* tmp_ip = ip;
					ip = dynamic_replace(ip, " -", "-");
					free(tmp_ip);
				}
				while(strstr(ip, "- ") != NULL)
				{
					char* tmp_ip = ip;
					ip = dynamic_replace(ip, "- ", "-");
					free(tmp_ip);
				}
				
				
				if( (strcmp(ip, "ALL_OTHERS_COMBINED") == 0 || strcmp(ip, "ALL_OTHERS_INDIVIDUAL") == 0) && (!process_other_quota)  )
				{
					push_list(other_quota_section_names, strdup(next_quota));
				}
				else
				{
					unsigned char is_individual_other =  strcmp(ip, "ALL_OTHERS_INDIVIDUAL") == 0 ? 1 : 0;
					if( strcmp(ip, "ALL_OTHERS_COMBINED") != 0 && strcmp(ip, "ALL_OTHERS_INDIVIDUAL") != 0 && strcmp(ip, "ALL") != 0 )
					{
						/* this is an explicitly defined ip or ip range, so save it for later, to deal with individual other overlap problem */
						push_list(defined_ip_groups, strdup(ip));
					}
					
					/* compute proper base id for rule, adding variable to uci if necessary */
					char* quota_base_id = get_uci_option(ctx, "firewall", next_quota, "id");
					if(quota_base_id == NULL)
					{
						char id_breaks[] = { ',', ' ', '\t'};
						unsigned long num_pieces;
						char** split_ip = split_on_separators(ip, id_breaks, 3, -1, 0, &num_pieces);
						char* first_ip = dynamic_replace(split_ip[0], "/", "_");
						free_null_terminated_string_array(split_ip);
						
						quota_base_id = strdup(first_ip);
						unsigned long next_postfix_count = 0;
						while( get_string_map_element(defined_base_ids, quota_base_id) != NULL)
						{
							char next_postfix[20];
							if(next_postfix_count > 25)
							{
								sprintf(next_postfix, "_%c", ('A' + next_postfix_count));
							}
							else
							{
								sprintf(next_postfix, "_Z%ld", (next_postfix_count - 25));
							}
							free(quota_base_id);
							quota_base_id = dynamic_strcat(2, first_ip, next_postfix);
						}
						free(first_ip);

						/* D for dummy place holder */
						set_string_map_element(defined_base_ids, quota_base_id, strdup("D"));
						
						/* add id we've decided on to UCI */
						char* var_set = dynamic_strcat(4, "firewall.", next_quota, ".id=", quota_base_id);
						if (uci_lookup_ptr(ctx, &ptr, var_set, true) == UCI_OK)
						{
							uci_set(ctx, &ptr);
						}
					}

					char* ignore_backup  = get_uci_option(ctx, "firewall", next_quota, "ignore_backup_at_next_restore");
					int do_restore = 1;
					if(ignore_backup != NULL)
					{
						do_restore = strcmp(ignore_backup, "1") == 0 ? 0 : 1;
						if(!do_restore)
						{
							//remove variable from uci 
							char* var_name = dynamic_strcat(3, "firewall.", next_quota, ".ignore_backup_at_next_restore");
							if (uci_lookup_ptr(ctx, &ptr, var_name, true) == UCI_OK)
							{
								uci_delete(ctx, &ptr);
							}
							free(var_name);
						}
					}
					free(ignore_backup);

					char* reset_interval = get_uci_option(ctx, "firewall", next_quota, "reset_interval");
					char* reset = strdup("");
					if(reset_interval != NULL)
					{
						char* reset_time     = get_uci_option(ctx, "firewall", next_quota, "reset_time");
						
						char* interval_option = strdup(" reset-interval ");
						reset = dcat_and_free(&reset, &interval_option, 1, 1);
						reset = dcat_and_free(&reset, &reset_interval, 1, 1);
						if(reset_time != NULL)
						{
							char* reset_option = strdup(" reset-time ");
							reset = dcat_and_free(&reset, &reset_option, 1, 1);
							reset = dcat_and_free(&reset, &reset_time, 1, 1);
						}
					}

					char* time_match_str = strdup("");	

					char* offpeak_hours     = get_uci_option(ctx, "firewall", next_quota, "offpeak_hours");
					char* offpeak_weekdays     = get_uci_option(ctx, "firewall", next_quota, "offpeak_weekdays");
					char* offpeak_weekly_ranges     = get_uci_option(ctx, "firewall", next_quota, "offpeak_weekly_ranges");

					char* onpeak_hours     = get_uci_option(ctx, "firewall", next_quota, "onpeak_hours");
					char* onpeak_weekdays     = get_uci_option(ctx, "firewall", next_quota, "onpeak_weekdays");
					char* onpeak_weekly_ranges     = get_uci_option(ctx, "firewall", next_quota, "onpeak_weekly_ranges");

					if(offpeak_hours != NULL || offpeak_weekdays != NULL || offpeak_weekly_ranges != NULL || onpeak_hours != NULL || onpeak_weekdays != NULL || onpeak_weekly_ranges != NULL)
					{
						unsigned char is_off_peak = (offpeak_hours != NULL || offpeak_weekdays != NULL || offpeak_weekly_ranges != NULL) ? 1 : 0;
						char* hours_var = is_off_peak ? offpeak_hours : onpeak_hours;
						char* weekdays_var = is_off_peak ? offpeak_weekdays : onpeak_weekdays;
						char* weekly_ranges_var = is_off_peak ? offpeak_weekly_ranges : onpeak_weekly_ranges;

						char *timerange_match = is_off_peak ? strdup(" timerange ! ") : strdup(" timerange ");
						char *hour_match = strdup(" hours \"");
						char *weekday_match = strdup(" weekdays \"");
						char *weekly_match = strdup(" weekly-ranges \"");
						char *quote_end = strdup("\" ");

						time_match_str = dcat_and_free(&time_match_str, &timerange_match,  1,1);
						if(hours_var != NULL && weekly_ranges_var == NULL)
						{
							time_match_str = dcat_and_free(&time_match_str, &hour_match, 1, 1);	
							time_match_str = dcat_and_free(&time_match_str, &hours_var, 1, 1);	
							time_match_str = dcat_and_free(&time_match_str, &quote_end, 1, 0);	
						}
						if(weekdays_var != NULL && weekly_ranges_var == NULL)
						{
							time_match_str = dcat_and_free(&time_match_str, &weekday_match, 1, 1);	
							time_match_str = dcat_and_free(&time_match_str, &weekdays_var, 1, 1);
							time_match_str = dcat_and_free(&time_match_str, &quote_end, 1, 0);	
						}
						if(weekly_ranges_var != NULL)
						{
							time_match_str = dcat_and_free(&time_match_str, &weekly_match, 1, 1);
							time_match_str = dcat_and_free(&time_match_str, &weekly_ranges_var, 1, 1);
							time_match_str = dcat_and_free(&time_match_str, &quote_end, 1, 0);
						}
						free(quote_end);
					}

					char* types[] = { "ingress_limit", "egress_limit", "combined_limit" };
					char* postfixes[] = { "_ingress", "_egress", "_combined" };
					char* chains[] =  { "ingress_quotas", "egress_quotas", "combined_quotas" };

					int type_index;
					for(type_index=0; type_index < 3; type_index++)
					{
						char* applies_to = strdup("combined");
						char* subnet_definition = strdup("");
						char* subnet6_definition = strdup("");

						char* limit = get_uci_option(ctx, "firewall", next_quota, types[type_index]);

						char* type_id = dynamic_strcat(2, quota_base_id, postfixes[type_index] );

						char* up_qos_mark = get_string_map_element(upload_qos_marks, exceeded_up_speed_str);
						char* down_qos_mark = get_string_map_element(download_qos_marks, exceeded_down_speed_str);
						if(full_qos_active)
						{
							up_qos_mark   = get_uci_option(ctx, "firewall", next_quota, "exceeded_up_class_mark");
							down_qos_mark = get_uci_option(ctx, "firewall", next_quota, "exceeded_down_class_mark");
						}

						/* 
						 * need to do ip test even if limit is null, because ALL_OTHERS quotas should not apply when any of the three types of explicit limit is defined
						 * and we therefore need to use this test to set mark indicating an explicit quota has been checked
						 */
						char* ip_test = strdup("");
						char* ip6_test = NULL;
						int foundip4 = 0;
						int foundip6 = 0;
						char* ip4_list = NULL;
						char* ip6_list = NULL;
						if( strcmp(ip, "ALL_OTHERS_COMBINED") != 0 && strcmp(ip, "ALL_OTHERS_INDIVIDUAL") != 0 && strcmp(ip, "ALL") != 0 )
						{
							char* src_test = dynamic_strcat(3, "saddr ", ip, " ");
							char* dst_test = dynamic_strcat(3, "daddr ", ip, " ");

							// Check IPv4 or IPv6 in the single IP case. Multiple IPs will be checked later
							if(strstr(ip, ",") == NULL && strstr(ip, " ") == NULL && strstr(ip, "\t") == NULL )
							{
								if(get_ipstr_family(ip) == 4)
								{
									foundip4 = 1;
									src_test = dynamic_strcat(2, " ip ", src_test);
									dst_test = dynamic_strcat(2, " ip ", dst_test);
								}
								else
								{
									foundip6 = 1;
									src_test = dynamic_strcat(2, " ip6 ", src_test);
									dst_test = dynamic_strcat(2, " ip6 ", dst_test);
								}
							}
							
							if(strstr(ip, ",") != NULL || strstr(ip, " ") != NULL || strstr(ip, "\t") != NULL )
							{
								char ip_breaks[] = { ',', ' ', '\t' };
								unsigned long num_ips = 0;
								char** ip_list = split_on_separators(ip, ip_breaks, 3, -1, 0, &num_ips);
								ip4_list = strdup("{");
								ip6_list = strdup("{");
								char ip4comma[2] = "";
								char ip6comma[2] = "";
								char* ip_list_end = strdup("}");
								unsigned long ip_index;
								
								for(ip_index=0; ip_index < num_ips; ip_index++)
								{
									char* tmpstr = NULL;
									char *next_ip = ip_list[ip_index];
									if(get_ipstr_family(next_ip) == 4)
									{
										tmpstr = dynamic_strcat(2, ip4comma, next_ip);
										ip4_list = dcat_and_free(&ip4_list, &tmpstr, 1, 1);
										sprintf(ip4comma, ",");
									}
									else
									{
										tmpstr = dynamic_strcat(2, ip6comma, next_ip);
										ip6_list = dcat_and_free(&ip6_list, &tmpstr, 1, 1);
										sprintf(ip6comma, ",");
									}
								}
								free(ip_list);
								
								ip4_list = dcat_and_free(&ip4_list, &ip_list_end, 1, 0);
								ip6_list = dcat_and_free(&ip6_list, &ip_list_end, 1, 1);
								
								if(strcmp(ip4_list, "{}") != 0)
								{
									char* egress_test = dynamic_strcat(3, " ip saddr ", ip4_list, " ");
									char* ingress_test = dynamic_strcat(3, " ip daddr ", ip4_list, " ");
									if(strcmp(types[type_index], "egress_limit") == 0)
									{
										run_shell_command(dynamic_strcat(7, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], egress_test, " ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0F000000 2>/dev/null"), 1);
									}
									else if(strcmp(types[type_index], "ingress_limit") == 0)
									{
										run_shell_command(dynamic_strcat(7, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], ingress_test, " ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0F000000 2>/dev/null"), 1);
									}
									else if(strcmp(types[type_index], "combined_limit") == 0)
									{
										run_shell_command(dynamic_strcat(9, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], " iifname ", wan_if, ingress_test, " ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0F000000 2>/dev/null"), 1);
										run_shell_command(dynamic_strcat(9, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], " oifname ", wan_if, egress_test, " ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0F000000 2>/dev/null"), 1);
									}
									free(ingress_test);
									foundip4 = 1;
								}
								else
								{
									free(ip4_list);
								}
								
								if(strcmp(ip6_list, "{}") != 0)
								{
									char* egress_test = dynamic_strcat(3, " ip6 saddr ", ip6_list, " ");
									char* ingress_test = dynamic_strcat(3, " ip6 daddr ", ip6_list, " ");
									if(strcmp(types[type_index], "egress_limit") == 0)
									{
										run_shell_command(dynamic_strcat(7, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], egress_test, " ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0F000000 2>/dev/null"), 1);
									}
									else if(strcmp(types[type_index], "ingress_limit") == 0)
									{
										run_shell_command(dynamic_strcat(7, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], ingress_test, " ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0F000000 2>/dev/null"), 1);
									}
									else if(strcmp(types[type_index], "combined_limit") == 0)
									{
										run_shell_command(dynamic_strcat(9, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], " iifname ", wan_if, ingress_test, " ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0F000000 2>/dev/null"), 1);
										run_shell_command(dynamic_strcat(9, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], " oifname ", wan_if, egress_test, " ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0F000000 2>/dev/null"), 1);
									}
									free(ingress_test);
									foundip6 = 1;
								}
								else
								{
									free(ip6_list);
								}
								
								char* rule_end = strdup(" ct mark \\& 0x0F000000 == 0x0F000000 ");
								ip_test = dcat_and_free(&ip_test, &rule_end, 1, 1);
							}
							else if(strcmp(types[type_index], "egress_limit") == 0)
							{
								ip_test = dcat_and_free(&ip_test, &src_test, 1, 0);
							}
							else if(strcmp(types[type_index], "ingress_limit") == 0)
							{
								ip_test = dcat_and_free(&ip_test, &dst_test, 1, 0);
							}
							else if(strcmp(types[type_index], "combined_limit") == 0)
							{
								run_shell_command(dynamic_strcat(9, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], " iifname ", wan_if, dst_test, " ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0F000000 2>/dev/null"), 1);
								run_shell_command(dynamic_strcat(9, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], " oifname ", wan_if, src_test, " ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0F000000 2>/dev/null"), 1);
								
								char* rule_end = strdup(" ct mark \\& 0x0F000000 == 0x0F000000 ");
								ip_test = dcat_and_free(&ip_test, &rule_end, 1, 1);
							}
							
							run_shell_command(dynamic_strcat(7, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], ip_test, " ct mark set ct mark \\& 0x0FFFFFFF \\| 0xF0000000 2>/dev/null"), 1);
							free(dst_test);
							free(src_test);
						}
						else if( strcmp(ip, "ALL_OTHERS_COMBINED") == 0 || strcmp(ip, "ALL_OTHERS_INDIVIDUAL") == 0 )
						{
							char* rule_end = strdup(" ct mark \\& 0xF0000000 == 0x0");
							char* subnet_end = strdup("\\\" ");
							ip_test = dcat_and_free(&ip_test, &rule_end, 1, 1);
							if(strcmp(ip, "ALL_OTHERS_INDIVIDUAL") == 0)
							{
								ip6_test=strdup(ip_test);
								free(applies_to);
								if(strcmp(types[type_index], "egress_limit") == 0)
								{
									char* subnet_test = dynamic_strcat(3, " ip saddr ", local_subnet, " ");
									ip_test = dcat_and_free(&subnet_test, &ip_test, 1, 1);
									subnet_test = dynamic_strcat(3, " ip6 saddr ", local_subnet6, " ");
									ip6_test = dcat_and_free(&subnet_test, &ip6_test, 1, 1);
									applies_to = strdup("individual_src");
								}
								else if(strcmp(types[type_index], "ingress_limit") == 0)
								{
									char* subnet_test = dynamic_strcat(3, " ip daddr ", local_subnet, " ");
									ip_test = dcat_and_free(&subnet_test, &ip_test, 1, 1);
									subnet_test = dynamic_strcat(3, " ip6 daddr ", local_subnet6, " ");
									ip6_test = dcat_and_free(&subnet_test, &ip6_test, 1, 1);
									applies_to = strdup("individual_dst");
								}
								else if(strcmp(types[type_index], "combined_limit") == 0)
								{
									applies_to = strdup("individual_local");
								}
									
								char *subnet_option = strdup(" subnet \\\"");
								char *subnet6_option = strdup(" subnet6 \\\"");
								subnet_definition = dcat_and_free(&subnet_definition, &subnet_option, 1, 1);
								subnet_definition = dcat_and_free(&subnet_definition, &local_subnet, 1, 0);
								subnet_definition = dcat_and_free(&subnet_definition, &subnet_end, 1, 0);
								subnet6_definition = dcat_and_free(&subnet6_definition, &subnet6_option, 1, 1);
								subnet6_definition = dcat_and_free(&subnet6_definition, &local_subnet6, 1, 0);
								subnet6_definition = dcat_and_free(&subnet6_definition, &subnet_end, 1, 1);
							}
							foundip4 = 1;
							foundip6 = 1;
						}
						else if(strcmp(ip, "ALL") == 0)
						{
							foundip4 = 1;
							foundip6 = 1;
						}
						if(ip6_test == NULL)
						{
							ip6_test=strdup(ip_test);
						}
						
						if(up_qos_mark != NULL && down_qos_mark != NULL)
						{
							char* set_egress_mark = dynamic_strcat(2, " meta mark set ", up_qos_mark);
							char* set_ingress_mark = dynamic_strcat(2, " meta mark set ", down_qos_mark);
							if(type_index == EGRESS_INDEX || type_index == INGRESS_INDEX)
							{
								int other_type_index = (type_index == EGRESS_INDEX ? INGRESS_INDEX : EGRESS_INDEX);
								char* other_limit = get_uci_option(ctx, "firewall", next_quota, types[other_type_index]);
								if(other_limit != NULL)
								{
									char* other_type_id = dynamic_strcat(2, quota_base_id, postfixes[other_type_index] );
									if(foundip4)
									{
										if(type_index == EGRESS_INDEX)
										{
											run_shell_command(dynamic_strcat(11, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], ip_test, time_match_str, " bandwidth id \\\"", other_type_id, "\\\" bcheck-with-src-dst-swap ", set_egress_mark), 1);
										}
										else
										{
											run_shell_command(dynamic_strcat(11, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], ip_test, time_match_str, " bandwidth id \\\"", other_type_id, "\\\" bcheck-with-src-dst-swap ", set_ingress_mark), 1);
										}
									}
									if(foundip6 && strcmp(ip_test, ip6_test) != 0)
									{
										if(type_index == EGRESS_INDEX)
										{
											run_shell_command(dynamic_strcat(11, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], ip6_test, time_match_str, " bandwidth id \\\"", other_type_id, "\\\" bcheck-with-src-dst-swap ", set_egress_mark), 1);
										}
										else
										{
											run_shell_command(dynamic_strcat(11, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], ip6_test, time_match_str, " bandwidth id \\\"", other_type_id, "\\\" bcheck-with-src-dst-swap ", set_ingress_mark), 1);
										}
									}
									free(other_type_id);
								}
								free(other_limit);
							}
							free(set_egress_mark);
							free(set_ingress_mark);
						}
						
						if(limit != NULL)
						{
							if(up_qos_mark != NULL && down_qos_mark != NULL)
							{
								char* set_egress_mark = dynamic_strcat(2, " meta mark set ", up_qos_mark);
								char* set_ingress_mark = dynamic_strcat(2, " meta mark set ", down_qos_mark);
								if(foundip4)
								{
									if(strcmp(types[type_index], "egress_limit") == 0)
									{
										run_shell_command(dynamic_strcat(17, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], ip_test, time_match_str, " bandwidth id \\\"", type_id, "\\\" type ", applies_to, subnet_definition, subnet6_definition, " greater-than ", limit, reset, set_egress_mark), 1);
									}
									else if(strcmp(types[type_index], "ingress_limit") == 0)
									{
										run_shell_command(dynamic_strcat(17, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], ip_test, time_match_str, " bandwidth id \\\"", type_id, "\\\" type ", applies_to, subnet_definition, subnet6_definition, " greater-than ", limit, reset, set_ingress_mark), 1);
									}
									else //combined
									{
										run_shell_command(dynamic_strcat(16, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], ip_test, time_match_str, " bandwidth id \\\"", type_id, "\\\" type ", applies_to, subnet_definition, subnet6_definition, " greater-than ", limit, reset), 1);
										run_shell_command(dynamic_strcat(14, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], " oifname ", wan_if, " ", ip_test, time_match_str, " bandwidth id \\\"", type_id, "\\\" bcheck ", set_egress_mark), 1);                     //egress
										run_shell_command(dynamic_strcat(14, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], " iifname ", wan_if, " ", ip_test, time_match_str, " bandwidth id \\\"", type_id, "\\\" bcheck-with-src-dst-swap ", set_ingress_mark), 1);  //ingress
									}
								}
								if(foundip6 && strcmp(ip_test, ip6_test) != 0)
								{
									if(strcmp(types[type_index], "egress_limit") == 0)
									{
										run_shell_command(dynamic_strcat(17, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], ip6_test, time_match_str, " bandwidth id \\\"", type_id, "\\\" type ", applies_to, subnet_definition, subnet6_definition, " greater-than ", limit, reset, set_egress_mark), 1);
									}
									else if(strcmp(types[type_index], "ingress_limit") == 0)
									{
										run_shell_command(dynamic_strcat(17, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], ip6_test, time_match_str, " bandwidth id \\\"", type_id, "\\\" type ", applies_to, subnet_definition, subnet6_definition, " greater-than ", limit, reset, set_ingress_mark), 1);
									}
									else //combined
									{
										run_shell_command(dynamic_strcat(16, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], ip6_test, time_match_str, " bandwidth id \\\"", type_id, "\\\" type ", applies_to, subnet_definition, subnet6_definition, " greater-than ", limit, reset), 1);
										run_shell_command(dynamic_strcat(14, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], " oifname ", wan_if, " ", ip6_test, time_match_str, " bandwidth id \\\"", type_id, "\\\" bcheck ", set_egress_mark), 1);                     //egress
										run_shell_command(dynamic_strcat(14, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], " iifname ", wan_if, " ", ip6_test, time_match_str, " bandwidth id \\\"", type_id, "\\\" bcheck-with-src-dst-swap ", set_ingress_mark), 1);  //ingress
									}
								}
								free(set_egress_mark);
								free(set_ingress_mark);
							}
							else
							{
								//insert quota block rule
								if(foundip4)
								{
									run_shell_command(dynamic_strcat(17, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], ip_test, time_match_str, " bandwidth id \\\"", type_id, "\\\" type ", applies_to, subnet_definition, subnet6_definition, " greater-than ", limit, reset, set_death_mark), 1);
								}
								if(foundip6 && strcmp(ip_test, ip6_test) != 0)
								{
									run_shell_command(dynamic_strcat(17, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], ip6_test, time_match_str, " bandwidth id \\\"", type_id, "\\\" type ", applies_to, subnet_definition, subnet6_definition, " greater-than ", limit, reset, set_death_mark), 1);
								}

								//insert redirect rule
								if(strcmp(ip, "ALL") == 0 || strcmp(ip, "ALL_OTHERS_INDIVIDUAL") == 0)
								{
									char* check_str = (strcmp(types[type_index], "ingress_limit") == 0) ? strdup(" bcheck-with-src-dst-swap ") : strdup(" bcheck ");
									run_shell_command(dynamic_strcat(9, "nft add rule ", quota_family_table, " nat_quota_redirects tcp dport {80,443} ", time_match_str, " bandwidth ", check_str, " id \\\"", type_id, "\\\" redirect"), 1);
									free(check_str);
								}
								else if(strcmp(ip, "ALL_OTHERS_COMBINED") == 0)
								{
									run_shell_command(dynamic_strcat(7, "nft add rule ", quota_family_table, " nat_quota_redirects tcp dport {80,443} ", time_match_str, " ct mark \\& 0xF0000000 == 0x0 bandwidth bcheck id \\\"", type_id, "\\\" redirect"), 1);
								}
								else
								{
									if(ip4_list != NULL)
									{
										run_shell_command(dynamic_strcat(5, "nft add rule ", quota_family_table, " nat_quota_redirects ip saddr ", ip4_list, " ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0F000000 2>/dev/null"), 1);
									}
									if(ip6_list != NULL)
									{
										run_shell_command(dynamic_strcat(5, "nft add rule ", quota_family_table, " nat_quota_redirects ip6 saddr ", ip6_list, " ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0F000000 2>/dev/null"), 1);
									}
									
									if(ip4_list == NULL && ip6_list == NULL)
									{
										run_shell_command(dynamic_strcat(7, "nft add rule ", quota_family_table, " nat_quota_redirects ", ((foundip6 && strcmp(ip_test, ip6_test) != 0) ? "ip6" : "ip"), " saddr ", ip, " ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0F000000 2>/dev/null"), 1);
									}
									run_shell_command(dynamic_strcat(7, "nft add rule ", quota_family_table, " nat_quota_redirects tcp dport {80,443} ", time_match_str, " ct mark \\& 0x0F000000 == 0x0F000000 bandwidth bcheck id \\\"", type_id, "\\\" redirect"), 1);
									run_shell_command(dynamic_strcat(3, "nft add rule ", quota_family_table, " nat_quota_redirects ct mark \\& 0x0F000000 == 0x0F000000 ct mark set ct mark \\& 0x0FFFFFFF \\| 0xF0000000 2>/dev/null"), 1);
									run_shell_command(dynamic_strcat(3, "nft add rule ", quota_family_table, " nat_quota_redirects ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0 2>/dev/null"), 1);
								}
							}

							//restore from backup
							if(do_restore)
							{
								restore_backup_for_id(type_id, "/usr/data/quotas", is_individual_other, defined_ip_groups);
							}
							free(limit);
						}
						if(strstr(ip_test, "ct mark") != NULL || strstr(ip6_test, "ct mark") != NULL)
						{
							run_shell_command(dynamic_strcat(6, "nft add rule ", quota_family_table, " ", quota_chain_prefix, chains[type_index], " ct mark set ct mark \\& 0xF0FFFFFF \\| 0x0 2>/dev/null"), 1);
						}

						free(ip_test);
						free(ip6_test);
						free(applies_to);
						free(subnet_definition);
						free(subnet6_definition);
						free(type_id);
						if(full_qos_active)
						{
							free(up_qos_mark);
							free(down_qos_mark);
						}
						if(ip4_list != NULL)
						{
							free(ip4_list);
						}
						if(ip6_list != NULL)
						{
							free(ip6_list);
						}
					}
					free(time_match_str);
					free(quota_base_id);
				}
				free(ip);
				free(exceeded_up_speed_str);
				free(exceeded_down_speed_str);
			}
			free(next_quota);
		}

		run_shell_command(dynamic_strcat(3, "nft add rule ", quota_family_table, " nat_quota_redirects ct mark set ct mark \\& 0x00FFFFFF \\| 0x0 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(5, "nft add rule ", quota_family_table, " ", quota_chain_prefix, "egress_quotas ct mark set ct mark \\& 0x00FFFFFF \\| 0x0 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(5, "nft add rule ", quota_family_table, " ", quota_chain_prefix, "ingress_quotas ct mark set ct mark \\& 0x00FFFFFF \\| 0x0 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(5, "nft add rule ", quota_family_table, " ", quota_chain_prefix, "combined_quotas ct mark set ct mark \\& 0x00FFFFFF \\| 0x0 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(7, "nft insert rule ", quota_family_table, " forward ct mark \\& ", death_mask, " == ", death_mark, " reject 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(7, "nft add rule ", quota_family_table, " ", quota_chain_prefix, "combined_quotas oifname ", wan_if, " meta mark \\& 0x7f != 0x0 ct mark set mark \\& 0x7f 2>/dev/null"), 1);
		run_shell_command(dynamic_strcat(7, "nft add rule ", quota_family_table, " ", quota_chain_prefix, "combined_quotas iifname ", wan_if, " meta mark \\& 0x7f00 != 0x0 ct mark set mark \\& 0x7f00 2>/dev/null"), 1);

		free(quota_family_table);

		//make sure crontab is up to date
		if(crontab_line != NULL)
		{
			FILE* crontab_file = fopen(crontab_file_path,"r");
			int cron_line_found = 0;
			if(crontab_file == NULL)
			{
				run_shell_command(dynamic_strcat(2, "mkdir -p ", crontab_dir), 1);
			}
			else
			{
				unsigned long read_length;
				char* all_cron_data = (char*)read_entire_file(crontab_file, 2048, &read_length);
				fclose(crontab_file);

				unsigned long num_lines;
				char linebreaks[] = { '\n', '\r' };
				char** cron_lines = split_on_separators(all_cron_data, linebreaks, 2, -1, 0, &num_lines);
				int line_index = 0;
				for(line_index=0; line_index < num_lines && (!cron_line_found); line_index++)
				{
					if(strcmp(cron_lines[line_index], crontab_line) == 0)
					{
						cron_line_found = 1;
					}
				}
				free_null_terminated_string_array(cron_lines);
			}
			if(!cron_line_found)
			{
				crontab_file = fopen(crontab_file_path, "a");
				fprintf(crontab_file, "%s\n", crontab_line);
				fclose(crontab_file);
			}
		}
	}
	else
	{
		//remove crontab line if it exists
		if(crontab_line != NULL)
		{
			FILE* crontab_file = fopen(crontab_file_path,"r");
			if(crontab_file != NULL)
			{
				unsigned long cron_line_found = 0;
				unsigned long read_length;
				unsigned long num_lines;
				char linebreaks[] = { '\n', '\r' };
				char* all_cron_data = (char*)read_entire_file(crontab_file, 2048, &read_length);
				fclose(crontab_file);
				char** cron_lines = split_on_separators(all_cron_data, linebreaks, 2, -1, 0, &num_lines);
				int line_index = 0;
				for(line_index=0; line_index < num_lines && (!cron_line_found); line_index++)
				{
					if(strcmp(cron_lines[line_index], crontab_line) == 0)
					{
						cron_line_found = 1;
					}
				}
				if(cron_line_found)
				{
					crontab_file = fopen(crontab_file_path, "w");
					for(line_index=0; line_index < num_lines; line_index++)
					{
						if(strcmp(cron_lines[line_index], crontab_line) != 0)
						{
							fprintf(crontab_file, "%s\n", cron_lines[line_index]);
						}
					}
					fclose(crontab_file);
				}
				free_null_terminated_string_array(cron_lines);
			}
		}
	}

	/* commit changes to uci, to remove ignore_backup_at_next_restore variables permanently */
	if (uci_lookup_ptr(ctx, &ptr, "firewall", true) == UCI_OK)
	{
		uci_commit(ctx, &ptr.p, false);
	}
	uci_free_context(ctx);

	return 0;
}

void restore_backup_for_id(char* id, char* quota_backup_dir, unsigned char is_individual_other, list* defined_ip_groups)
{
	char* quota_file_path = dynamic_strcat(3, quota_backup_dir, "/quota_", id);
	unsigned long num_ips = 0;
	time_t last_backup = 0;
	ip_bw* loaded_backup_data = load_usage_from_file(quota_file_path, &num_ips, &last_backup);
	if(loaded_backup_data != NULL)
	{
		//printf("restoring quota... id=%s, is_individual_other=%d, num_defined_ip_groups=%d\n", id, is_individual_other, defined_ip_groups->length);
		if(is_individual_other)
		{
			//filter out any ips in the "other" data, that have now been assigned quotas of their own.
			list* ip_bw_list = initialize_list();
			int ip_index;
			for(ip_index=0; ip_index < num_ips; ip_index++)
			{
				ip_bw* ptr = loaded_backup_data + ip_index;
				push_list(ip_bw_list, ptr);
			}
			
			unsigned long num_groups = 0;
			char** group_strs = (char**)get_list_values(defined_ip_groups, &num_groups);
			unsigned long group_index;
			
			for(group_index = 0; group_index < num_groups; group_index++)
			{
				filter_group_from_list(&ip_bw_list, group_strs[group_index]);
			}
			
			
			//rebuild the backup data array from the filtered list
			if(num_ips != ip_bw_list->length)
			{
				num_ips = ip_bw_list->length;
				ip_bw* adj_backup = (ip_bw*)malloc( (1+ip_bw_list->length)*sizeof(ip_bw) );
				while(ip_bw_list->length > 0)
				{
					ip_bw* next = pop_list(ip_bw_list);
					adj_backup[ ip_bw_list->length ] = *next;
				}
				free(loaded_backup_data);
				loaded_backup_data = adj_backup;
			}
			
			destroy_list(ip_bw_list, DESTROY_MODE_IGNORE_VALUES, &num_groups);
			free(group_strs); //don't want to destroy values, they're still contained in list, so just destroy container array
		}
		set_bandwidth_usage_for_rule_id(id, 1, num_ips, last_backup, loaded_backup_data, 5000);
	}
	free(quota_file_path);
}

list* filter_group_from_list(list** orig_ip_bw_list, char* ip_group_str)
{
	char* dyn_group_str = strdup(ip_group_str);

	/* remove spaces in ip range definitions */
	while(strstr(dyn_group_str, " -") != NULL)
	{
		char* tmp_group_str = dyn_group_str;
		dyn_group_str = dynamic_replace(dyn_group_str, " -", "-");
		free(tmp_group_str);
	}
	while(strstr(dyn_group_str, "- ") != NULL)
	{
		char* tmp_group_str = dyn_group_str;
		dyn_group_str = dynamic_replace(dyn_group_str, "- ", "-");
		free(tmp_group_str);
	}
	while(strstr(dyn_group_str, " -") != NULL)
	{
		char* tmp_group_str = dyn_group_str;
		dyn_group_str = dynamic_replace(dyn_group_str, " /", "/");
		free(tmp_group_str);
	}
	while(strstr(dyn_group_str, "- ") != NULL)
	{
		char* tmp_group_str = dyn_group_str;
		dyn_group_str = dynamic_replace(dyn_group_str, "/ ", "/");
		free(tmp_group_str);
	}

	char group_breaks[]= ",\t ";
	unsigned long num_groups = 0;
	char** split_group = split_on_separators(dyn_group_str, group_breaks, 3, -1, 0, &num_groups);
	unsigned long group_index;
	
	for(group_index = 0; group_index < num_groups; group_index++)
	{
		int ip_family = 0;
		uint32_t* range = ip_range_to_host_ints( split_group[group_index], &ip_family );
		list* new_ip_bw_list = initialize_list();
		while((*orig_ip_bw_list)->length > 0)
		{
			ip_bw* next_ip_bw = shift_list(*orig_ip_bw_list);
			uint32_t* test_ip = next_ip_bw->ip;
			if(ip_family == AF_INET)
			{
				if(ntohl(*test_ip) >= ntohl(range[0]) && ntohl(*test_ip) <= ntohl(range[4]))
				{
					//overlap found! filter the ip by not adding it back!
				}
				else
				{
					push_list(new_ip_bw_list, next_ip_bw);
				}
			}
			else
			{
				if(memcmp(test_ip,range,sizeof(uint32_t)*4) >= 0 && memcmp(test_ip,range+4,sizeof(uint32_t)*4) <= 0)
				{
					//overlap found! filter the ip by not adding it back!
				}
				else
				{
					push_list(new_ip_bw_list, next_ip_bw);
				}
			}
		}
		free(range);
		unsigned long num_destroyed;
		destroy_list(*orig_ip_bw_list, DESTROY_MODE_IGNORE_VALUES, &num_destroyed);
		*orig_ip_bw_list = new_ip_bw_list;
	}
	free_null_terminated_string_array(split_group);
	free(dyn_group_str);

	return *orig_ip_bw_list;

}

uint32_t* ip_range_to_host_ints(char* ip_str, int* family)
{
	uint32_t* ret_val = (uint32_t*)malloc(8*sizeof(uint32_t));
	uint32_t* start = NULL;
	uint32_t* end = NULL;
	
	int ip_family = 0;
	unsigned long num_pieces = 0;
	char ip_breaks[] = "/-";
	char** split_ip = split_on_separators(ip_str, ip_breaks, 2, -1, 0, &num_pieces);
	start = ip_to_host_int(split_ip[0], &ip_family);
	*family = ip_family;
	if(ip_family == AF_INET)
	{
		if(strstr(ip_str, "/") != NULL)
		{
			uint32_t mask = 0;
			uint32_t* tmask = NULL;
			if(strstr(split_ip[1], ".") != NULL)
			{
				tmask = ip_to_host_int(split_ip[1], &ip_family);
				mask = *tmask;
				free(tmask);
			}
			else
			{
				uint32_t mask_size;
				sscanf(split_ip[1], "%d", &mask_size);
				mask = htonl(0xFFFFFFFF << (32-mask_size));
			}
			*start = *start & mask;
			*end = *start | ( ~mask );
		}
		else if(strstr(ip_str, "-") != NULL)
		{
			end = ip_to_host_int(split_ip[1], &ip_family);
		}
		else
		{
			end = start;
		}
	}
	else
	{
		if(strstr(ip_str, "/") != NULL)
		{
			uint32_t mask[4];
			uint32_t* tmask = NULL;
			if(strstr(split_ip[1], ":") != NULL)
			{
				tmask = ip_to_host_int(split_ip[1], &ip_family);
				mask[0] = *tmask;
				mask[1] = *(tmask+1);
				mask[2] = *(tmask+2);
				mask[3] = *(tmask+3);
				free(tmask);
			}
			else
			{
				uint32_t mask_size;
				sscanf(split_ip[1], "%d", &mask_size);
				char* p = (void *)&mask;
				memset(p, 0xFF, mask_size/8);
				memset(p + ((mask_size+7)/8), 0, (128-mask_size)/8);
				if(mask_size < 128)
				{
					p[mask_size/8] = 0xFF << (8-(mask_size & 7));
				}
			}
			for(int x = 0; x < 4; x++)
			{
				*start = *(start+x) & mask[x];
				*end = *(start+x) | (~mask[x]);
			}
		}
		else if(strstr(ip_str, "-") != NULL)
		{
			end = ip_to_host_int(split_ip[1], &ip_family);
		}
		else
		{
			end = start;
		}
	}
	
	free_null_terminated_string_array(split_ip);

	memcpy(ret_val, start, 4*sizeof(uint32_t));
	memcpy(ret_val+4, end, 4*sizeof(uint32_t));
	if(end != start)
	{
		free(end);
	}
	free(start);
	return ret_val;
}

uint32_t* ip_to_host_int(char* ip_str, int* family)
{
	int ret = 0;
	uint32_t* retval = malloc(4*sizeof(uint32_t));
	*family = 0;
	struct in6_addr inp6;
	ret = inet_pton(AF_INET6, ip_str, &inp6);
	if(ret == 1)
	{
		*retval = inp6.s6_addr32[0];
		*(retval+1) = inp6.s6_addr32[1];
		*(retval+2) = inp6.s6_addr32[2];
		*(retval+3) = inp6.s6_addr32[3];
		*family = AF_INET6;
	}
	else
	{
		struct in_addr inp;
		ret = inet_pton(AF_INET, ip_str, &inp);
		*retval = inp.s_addr;
		*(retval+1) = 0;
		*(retval+2) = 0;
		*(retval+3) = 0;
		*family = AF_INET;
	}
	return retval;
}

int get_ipstr_family(char* ip_str)
{
	int retVal = 4;
	int ret = 0;
	struct in6_addr ipaddr;
	char ip_breaks[] = { '-' };
	unsigned long num_ips = 0;
	char** ip_list = split_on_separators(ip_str, ip_breaks, 1, -1, 0, &num_ips);
	//Always test first IP. This covers the single and range cases.
	ret = inet_pton(AF_INET6, ip_list[0], &ipaddr);
	if(ret == 1)
	{
		// Assume failure means IP is IPv4. If we are getting bad IPs passed in here that is a whole different problem...
		retVal = 6;
	}
	
	return retVal;
}

// Accepts bitmask as either hex or decimal string (e.g. 0xFF00 or 65280) and returns the inverted bitmask in hex (0x00FF)
char* invert_bitmask(const char* input)
{
    if (!input || *input == '\0')
	{
        return NULL;
    }

    uint32_t value = 0;
    int num_bits = 0;
    int is_hex = 0;

    // Check if input starts with "0x" or "0X"
    if (strncmp(input, "0x", 2) == 0 || strncmp(input, "0X", 2) == 0)
	{
        is_hex = 1;
        const char* hex = input + 2;
        size_t len = strlen(hex);
        if (len > 8) return NULL; // max 32 bits (8 hex digits)

        while (*hex)
		{
            char c = *hex++;
            value <<= 4;
            if (c >= '0' && c <= '9') value |= (c - '0');
            else if (c >= 'a' && c <= 'f') value |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= (c - 'A' + 10);
            else return NULL;
        }

        num_bits = len * 4; // each hex digit is 4 bits
    }
	else
	{
        // Parse as decimal
        char* endptr = NULL;
        unsigned long parsed = strtoul(input, &endptr, 10);
        if (*endptr != '\0' || parsed > 0xFFFFFFFFUL)
		{
            return NULL; // invalid or too large for 32-bit
        }
        value = (uint32_t)parsed;

        // Determine how many bits are needed to represent the value
        for (int i = 31; i >= 0; i--)
		{
            if (value & (1U << i))
			{
                num_bits = i + 1;
                break;
            }
        }
        if (num_bits == 0) num_bits = 1; // handle value == 0
    }

    uint32_t mask = (num_bits == 32) ? 0xFFFFFFFF : ((1U << num_bits) - 1);
    uint32_t inverted = ~value & mask;

    // Allocate result string: "0x" + 8 hex digits + null terminator
    char* result = malloc(2 + 8 + 1);
    if (!result) return NULL;

    sprintf(result, "0x%0*X", (num_bits + 3) / 4, inverted); // pad to full hex digits
    return result;
}

void delete_chain_from_table(char* family, char* table, char* delete_chain)
{
	char *command = dynamic_strcat(5, "nft -a list table ", family, " ", table, " 2>/dev/null");
	unsigned long num_lines = 0;
	char** table_dump = get_shell_command_output_lines(command, &num_lines);
	free(command);

	unsigned long line_index;
	char* current_chain = NULL;
	list* delete_commands = initialize_list();

	for(line_index=0; line_index < num_lines; line_index++)
	{
		char* line = table_dump[line_index];
		unsigned long num_pieces = 0;
		char whitespace[] = { '\t', ' ', '\r', '\n' };
		char** line_pieces = split_on_separators(line, whitespace, 4, -1, 0, &num_pieces);
		if(strcmp(line_pieces[0], "chain") == 0)
		{
			if(current_chain != NULL) { free(current_chain); }
			current_chain = strdup(line_pieces[1]);
		}
		else 
		{
			if(current_chain != NULL && num_pieces > 1)
			{
				unsigned long pieceidx = 0;
				for(pieceidx = 0; pieceidx < num_pieces; pieceidx++)
				{
					if((strcmp(line_pieces[pieceidx], "jump") == 0 || strcmp(line_pieces[pieceidx], "goto") == 0) && (pieceidx < num_pieces - 1))
					{
						if(strcmp(line_pieces[pieceidx+1], delete_chain) == 0)
						{
							char* delete_command = dynamic_strcat(9, "nft delete rule ", family, " ", table, " ", current_chain, " handle ", line_pieces[num_pieces-1], " 2>/dev/null");
							push_list(delete_commands, delete_command);
							break;
						}
					}
				}
			}
		}

		//free line_pieces
		free_null_terminated_string_array(line_pieces);
	}
	free_null_terminated_string_array(table_dump);

	/* final two commands to flush chain being deleted and whack it */
	unshift_list(delete_commands, dynamic_strcat(7, "nft flush chain ", family, " ", table, " ", delete_chain, " 2>/dev/null"));
	unshift_list(delete_commands, dynamic_strcat(7, "nft delete chain ", family, " ", table, " ", delete_chain, " 2>/dev/null"));

	/* run delete commands */
	while(delete_commands->length > 0)
	{
		char *next_command = (char*)pop_list(delete_commands);
		run_shell_command(next_command, 1);
	}
}

void run_shell_command(char* command, int free_command_str)
{
	if(dry_run)
	{
		printf("%s\n", command);
	}
	else
	{
		system(command);
	}
	if(free_command_str)
	{
		free(command);
	}
}

list* get_all_sections_of_type(struct uci_context *ctx, char* package, char* section_type)
{

	struct uci_package *p = NULL;
	struct uci_element *e = NULL;

	list* sections_of_type = initialize_list();
	if(uci_load(ctx, package, &p) == UCI_OK)
	{
		uci_foreach_element( &p->sections, e)
		{
			struct uci_section *section = uci_to_section(e);
			if(safe_strcmp(section->type, section_type) == 0)
			{
				push_list(sections_of_type, strdup(section->e.name));
			}
		}
	}
	return sections_of_type;
}


char* get_uci_option(struct uci_context* ctx, char* package_name, char* section_name, char* option_name)
{
	char* option_value = NULL;
	struct uci_ptr ptr;
	char* lookup_str = dynamic_strcat(5, package_name, ".", section_name, ".", option_name);
	int ret_value = uci_lookup_ptr(ctx, &ptr, lookup_str, 1);
	if(ret_value == UCI_OK)
	{
		if( !(ptr.flags & UCI_LOOKUP_COMPLETE))
		{
			ret_value = UCI_ERR_NOTFOUND;
		}
		else
		{
			struct uci_element *e = (struct uci_element*)ptr.o;
			option_value = get_option_value_string(uci_to_option(e));
		}
	}
	free(lookup_str);

	return option_value;
}

// this function dynamically allocates memory for
// the option string, but since this program exits
// almost immediately (after printing variable info)
// the massive memory leak we're opening up shouldn't
// cause any problems.  This is your reminder/warning
// that this might be an issue if you use this code to
// do anything fancy.
char* get_option_value_string(struct uci_option* uopt)
{
	char* opt_str = NULL;
	if(uopt->type == UCI_TYPE_STRING)
	{
		opt_str = strdup(uopt->v.string);
	}
	if(uopt->type == UCI_TYPE_LIST)
	{
		struct uci_element* e;
		uci_foreach_element(&uopt->v.list, e)
		{
			if(opt_str == NULL)
			{
				opt_str = strdup(e->name);
			}
			else
			{
				char* tmp;
				tmp = dynamic_strcat(3, opt_str, " ", e->name);
				free(opt_str);
				opt_str = tmp;
			}
		}
	}

	return opt_str;
}

