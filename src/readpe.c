#include "src/readpe_internal.h"


static inline bool is_last_image_import_descriptor(PE_Image_Import_Descriptor* descriptor)
{
    return descriptor->first_thunk == 0 && descriptor->forwarder_chain == 0 && descriptor->name == 0 && descriptor->something.original_first_thunk == 0 && descriptor->timestamp == 0;
}

static bool read_import_table(FILE* pe_file, PE_Information* megastructure_information)
{
    bool last_import_desc = false;

    fseek(pe_file, megastructure_information->directory_addresses[IMAGE_DIRECTORY_ENTRY_IMPORT].address, SEEK_SET);
    assert(is_seek_forward(ftell(pe_file)));

    megastructure_information->image_imports = NULL;
    while (!last_import_desc)
    {
        PE_Image_Import_Descriptor import_descriptor;

        if(fread(&import_descriptor, sizeof(PE_Image_Import_Descriptor), 1, pe_file) <= 0)
        {
            return false;
        }

        last_import_desc = is_last_image_import_descriptor(&import_descriptor);

        if(!last_import_desc)
        {
            megastructure_information->image_imports = realloc(megastructure_information->image_imports, (megastructure_information->image_import_count + 1) * sizeof(PE_Image_Import_Descriptor));
            megastructure_information->image_imports[megastructure_information->image_import_count] = import_descriptor;

            megastructure_information->image_imports[megastructure_information->image_import_count].name = find_offset_from_rva(megastructure_information->section_count, megastructure_information->section_headers, import_descriptor.name);
            megastructure_information->image_imports[megastructure_information->image_import_count].something.original_first_thunk = find_offset_from_rva(megastructure_information->section_count, megastructure_information->section_headers, import_descriptor.something.original_first_thunk);

            megastructure_information->image_import_count++;
        }
    }

    if (megastructure_information->image_import_count != 0)
    {
        megastructure_information->import_dll_names = calloc(megastructure_information->image_import_count, sizeof(char*));
        megastructure_information->import_function_names = calloc(megastructure_information->image_import_count, sizeof(char**));
        megastructure_information->image_lookup_descriptors = calloc(megastructure_information->image_import_count, sizeof(uint32_t*));
    }
    return true;
}


static bool read_import_lookup_descriptors(FILE* pe_file, PE_Information* megastructure_information, uint32_t import_index)
{
    fseek(pe_file, megastructure_information->image_imports[import_index].something.original_first_thunk, SEEK_SET);
    assert(is_seek_forward(ftell(pe_file)));

    int64_t x;
    int count = 0;
    megastructure_information->image_lookup_descriptors[import_index] = malloc(0);
    do
    {
        x = read_lookup_descriptor(pe_file, megastructure_information);

        if(x == -1)
        {
            return false;
        }
        if(x == -2)
        {
            continue;
        }

        megastructure_information->image_lookup_descriptors[import_index] = (uint32_t*) realloc(megastructure_information->image_lookup_descriptors[import_index], (count + 1) * sizeof(uint32_t));
        
        megastructure_information->image_lookup_descriptors[import_index][count] = (uint32_t)x;
        count++;
    } while(x != (uint32_t)(-1));

    megastructure_information->import_function_names[import_index] = calloc(count, sizeof(char*));
    return true;
}

static bool read_export_function_name_pointers(FILE* pe_file, PE_Information* megastructure_information)
{
    fseek(pe_file, megastructure_information->image_export.name, SEEK_SET);
    assert(is_seek_forward(ftell(pe_file)));

    if(fread(megastructure_information->export_module_function_pointers, sizeof(uint32_t), megastructure_information->image_export.name_count, pe_file) <= 0)
    {
        return false;
    }
    for (uint32_t i = 0; i < megastructure_information->image_export.name_count; i++)
    {
        megastructure_information->export_module_function_pointers[i] = find_offset_from_rva(megastructure_information->section_count, megastructure_information->section_headers, megastructure_information->export_module_function_pointers[i]);
    }

    return true;
}


static bool read_next_data(FILE* pe_file, PE_Information* megastructure_information)
{
    while (true)
    {
        uint64_t min_address = (uint64_t) -1;
        for (int i = 0; i < IMAGE_DIRECTORY_ENTRY_NB_ARGS; i++)
        {
            if (megastructure_information->directory_addresses[i].address != 0 && megastructure_information->directory_addresses[i].address < min_address)
            {
                min_address = megastructure_information->directory_addresses[i].address;
            }
        }
        if (megastructure_information->image_imports != NULL)
        {
            for (int i = 0; i < megastructure_information->image_import_count; i++)
            {
                if (megastructure_information->image_imports[i].something.original_first_thunk != 0 && megastructure_information->image_imports[i].something.original_first_thunk < min_address)
                {
                    min_address = megastructure_information->image_imports[i].something.original_first_thunk;
                }
                if (megastructure_information->image_imports[i].name != 0 && megastructure_information->image_imports[i].name < min_address)
                {
                    min_address = megastructure_information->image_imports[i].name;
                }
                if (megastructure_information->image_lookup_descriptors[i] != NULL)
                {
                    for (uint32_t j = 0; megastructure_information->image_lookup_descriptors[i][j] != (uint32_t) -1; j++)
                    {
                        if (megastructure_information->image_lookup_descriptors[i][j] != 0 && megastructure_information->image_lookup_descriptors[i][j] < min_address)
                        {
                            min_address = megastructure_information->image_lookup_descriptors[i][j];
                        }
                    }
                }
            }
        }
        if (megastructure_information->image_export.name != 0 && megastructure_information->image_export.name < min_address)
        {
            min_address = megastructure_information->image_export.name;
        }
        if (megastructure_information->image_export.name_pointer != 0 && megastructure_information->image_export.name_pointer < min_address)
        {
            min_address = megastructure_information->image_export.name_pointer;
        }
        if (megastructure_information->export_module_function_pointers != NULL)
        {
            for (uint32_t i = 0; i < megastructure_information->image_export.name_count; i++)
            {
                if (megastructure_information->export_module_function_pointers[i] != 0 && megastructure_information->export_module_function_pointers[i] < min_address)
                {
                    megastructure_information->export_module_function_pointers[i] = min_address;
                }
            }
        }
        if (min_address == (uint64_t) -1)
        {
            return true;
        }
        for (int i = 0; i < IMAGE_DIRECTORY_ENTRY_NB_ARGS; i++)
        {
            if (megastructure_information->directory_addresses[i].address == min_address)
            {
                switch (i)
                {
                    case 0:
                        if(!read_export_directory(pe_file, megastructure_information))
                        {
                            return false;
                        }
                        megastructure_information->directory_addresses[i].address = 0;
                        break;
                    case 1:
                        if(!read_import_table(pe_file, megastructure_information))
                        {
                            return false;
                        }
                        megastructure_information->directory_addresses[i].address = 0;
                        break;
                    case 4:
                        if(!read_certificate(pe_file, megastructure_information))
                        {
                            return false;
                        }
                        megastructure_information->directory_addresses[i].address = 0;
                        break;
                }
                goto NEXT_ITERATION;
            }
        }
        if (megastructure_information->image_imports != NULL)
        {
            for (uint16_t i = 0; i < megastructure_information->image_import_count; i++)
            {
                if (megastructure_information->image_imports[i].something.original_first_thunk == min_address)
                {
                    if(!read_import_lookup_descriptors(pe_file, megastructure_information, i))
                    {
                        return false;
                    }
                    megastructure_information->image_imports[i].something.original_first_thunk = 0;
                    goto NEXT_ITERATION;
                }
                if (megastructure_information->image_imports[i].name == min_address)
                {
                    if(!read_import_dll_name(pe_file, megastructure_information, i))
                    {
                        return false;
                    }
                    megastructure_information->image_imports[i].name = 0;
                    goto NEXT_ITERATION;
                }
                if (megastructure_information->image_lookup_descriptors[i] != NULL)
                {
                    for (uint32_t j = 0; megastructure_information->image_lookup_descriptors[i][j] != (uint32_t) -1; j++)
                    {
                        if (megastructure_information->image_lookup_descriptors[i][j] == min_address)
                        {
                            if(!read_import_function_name(pe_file, megastructure_information, i, j))
                            {
                                return false;
                            }
                            megastructure_information->image_lookup_descriptors[i][j] = 0;
                            goto NEXT_ITERATION;
                        }
                    }
                }
            }
        }
        if (megastructure_information->image_export.name == min_address)
        {
            if(!read_export_module_name(pe_file, megastructure_information))
            {
                return false;
            }
            megastructure_information->image_export.name = 0;
            goto NEXT_ITERATION;
        }
        if (megastructure_information->image_export.name_pointer == min_address)
        {
            if(!read_export_function_name_pointers(pe_file, megastructure_information))
            {
                return false;
            }
            megastructure_information->image_export.name_pointer = 0;
            goto NEXT_ITERATION;
        }
        if (megastructure_information->export_module_function_pointers != NULL)
        {
            for (uint32_t i = 0; i < megastructure_information->image_export.name_count; i++)
            {
                if (megastructure_information->export_module_function_pointers[i] == min_address)
                {
                    if(!read_export_function_name(pe_file, megastructure_information, i))
                    {
                        return false;
                    }
                    megastructure_information->export_module_function_pointers[i] = 0;
                    goto NEXT_ITERATION;
                }
            }
        }
NEXT_ITERATION:
    }
}

PE_Information* read_pe(const char* filename)
{
    PE_Information* megastructure_information = NULL;
    PE_Optional_Header pe_optional_header;
    pe_optional_header.data_directory = NULL;
    PE_COFF_Header coff_header;
    PE_DOS_Header dos_header;
    FILE* pe_file = fopen(filename, "r");
    if (pe_file == NULL)
    {
        fputs("Error: can't open file\n", stderr);
        goto ERROR;
    }
    if (!read_dos_header(pe_file, &dos_header))
    {
        fputs("Error: invalid DOS header\n", stderr);
        goto ERROR;
    }
    fseek(pe_file, dos_header.lfa_new, SEEK_SET);
    assert(is_seek_forward(ftell(pe_file)));
    if (!read_coff_header(pe_file, &coff_header))
    {
        fputs("Error: invalid COFF header\n", stderr);
        goto ERROR;
    }
    if (coff_header.optional_header_size == 0)
    {
        fputs("Error: this file doesn't have an optional header, I don't know how to proceed\n", stderr);
        goto ERROR;
    }
    if(fread(&pe_optional_header, offsetof(PE_Optional_Header, stack_reserved_size), 1, pe_file) <= 0)
    {
        fputs("Error: file corrupted\n", stderr);
        goto ERROR;
    }
    if (pe_optional_header.signature == PE_OPTIONAL_HEADER_SIGNATURE_64)
    {
        if(fread(&(pe_optional_header.stack_reserved_size), sizeof(uint64_t), 1, pe_file) <= 0
            || fread(&(pe_optional_header.stack_commit_size), sizeof(uint64_t), 1, pe_file) <= 0
            || fread(&(pe_optional_header.heap_reserve_size), sizeof(uint64_t), 1, pe_file) <= 0
            || fread(&(pe_optional_header.heap_commit_size), sizeof(uint64_t), 1, pe_file) <= 0)
        {
            fputs("Error: file corrupted\n", stderr);
            goto ERROR;
        }
    }
    else if (pe_optional_header.signature == PE_OPTIONAL_HEADER_SIGNATURE_32)
    {
        if(fread(&(pe_optional_header.stack_reserved_size), sizeof(uint32_t), 1, pe_file) <= 0
            || fread(&(pe_optional_header.stack_commit_size), sizeof(uint32_t), 1, pe_file) <= 0
            || fread(&(pe_optional_header.heap_reserve_size), sizeof(uint32_t), 1, pe_file) <= 0
            || fread(&(pe_optional_header.heap_commit_size), sizeof(uint32_t), 1, pe_file) <= 0)
        {
            fputs("Error: file corrupted\n", stderr);
            goto ERROR;
        }
    }
    else
    {
        fputs("Error: this tool only supports PE executable files\n", stderr);
        goto ERROR;
    }

    megastructure_information = (PE_Information*) calloc(1, sizeof(PE_Information));

    if(fread(&(pe_optional_header.loader_flags), sizeof(uint32_t), 1, pe_file) <= 0
        || fread(&(pe_optional_header.rva_number_size), sizeof(uint32_t), 1, pe_file) <= 0
        || fread(megastructure_information->directory_addresses, sizeof(PE_Data_Directory), IMAGE_DIRECTORY_ENTRY_NB_ARGS, pe_file) <= 0)
    {
        fputs("Error: file corrupted\n", stderr);
        goto ERROR;
    }

    if (pe_optional_header.rva_number_size > IMAGE_DIRECTORY_ENTRY_NB_ARGS)
    {
        fseek(pe_file, (pe_optional_header.rva_number_size - IMAGE_DIRECTORY_ENTRY_NB_ARGS) * sizeof(PE_Data_Directory), SEEK_CUR);
        assert(is_seek_forward(ftell(pe_file)));
    }

    megastructure_information->section_headers = malloc(coff_header.section_count * sizeof(PE_Section_Header));
    if (megastructure_information->section_headers == NULL)
    {
        fputs("An error has occurred\n", stderr);
        goto ERROR;
    }
    if(fread(megastructure_information->section_headers, sizeof(PE_Section_Header), coff_header.section_count, pe_file) <= 0)
    {
        fputs("Error: file corrupted\n", stderr);
        goto ERROR;
    }

    megastructure_information->section_count = coff_header.section_count;

    for (int i = 0; i < IMAGE_DIRECTORY_ENTRY_NB_ARGS; i++)
    {
        if (i != 4)
        {
            megastructure_information->directory_addresses[i].address = find_offset_from_rva(coff_header.section_count, megastructure_information->section_headers, megastructure_information->directory_addresses[i].address);
        }

        // Temporary because I can't parse other information
        if (i != 0 && i != 1 && i != 4)
        {
            megastructure_information->directory_addresses[i].address = 0;
        }
    }

    megastructure_information->bits_64 = (pe_optional_header.signature == PE_OPTIONAL_HEADER_SIGNATURE_64);

    if(!read_next_data(pe_file, megastructure_information))
    {
        fputs("Error: file corrupted\n", stderr);
        goto ERROR;
    }

    FILE* cert = fopen("certificate.der", "wb");
    if (cert != NULL)
    {
        fwrite(megastructure_information->signature, sizeof(uint8_t), megastructure_information->signature_length, cert);
        fclose(cert);
    }
    else
    {
        fputs("Can't write certificate file to certificate.der, ignoring\n", stderr);
    }


    if(pe_file != NULL)
        fclose(pe_file);

    return megastructure_information;

ERROR:
    free_megastructure(&megastructure_information);
    if(pe_file != NULL)
        fclose(pe_file);
    
    return NULL;
}


void free_megastructure(PE_Information** pps)
{
    if(*pps == NULL)
    {
        return;
    }

    free((*pps)->signature);
    free((*pps)->section_headers);
    if ((*pps)->import_dll_names != NULL)
    {
        for (uint32_t i = 0; i < (*pps)->image_import_count; i++)
        {
            free((*pps)->import_dll_names[i]);
        }
    }
    if ((*pps)->import_function_names != NULL)
    {
        for (uint32_t i = 0; i < (*pps)->image_import_count && (*pps)->import_function_names[i] != NULL; i++)
        {
            for (uint32_t j = 0; (*pps)->import_function_names[i][j] != NULL; j++)
            {
                free((*pps)->import_function_names[i][j]);
            }
            free((*pps)->import_function_names[i]);
        }
    }
    if ((*pps)->image_lookup_descriptors != NULL)
    {
        for (uint32_t i = 0; i < (*pps)->image_import_count; i++)
        {
            free((*pps)->image_lookup_descriptors[i]);
        }
    }
    if ((*pps)->export_module_functions != NULL)
    {
        for (uint32_t i = 0; i < (*pps)->image_export.function_count; i++)
        {
            free((*pps)->export_module_functions[i]);
        }
    }
    for (uint32_t i = 0; i < (*pps)->image_export.name_count; i++)
    {
        free((*pps)->export_module_functions[i]);
    }
    free((*pps)->export_module_functions);
    free((*pps)->export_module_name);
    free((*pps)->image_imports);
    free((*pps)->import_dll_names);
    free((*pps)->import_function_names);
    free((*pps)->image_lookup_descriptors);

    free(*pps);
    *pps = NULL;
}