/*	MCM file compressor

  Copyright (C) 2014, Google Inc.
  Authors: Mathieu Chartier

  LICENSE

    This file is part of the MCM file compressor.

    MCM is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    MCM is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with MCM.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "Archive.hpp"
#include <future>
#include <mutex>

#include <algorithm>
#include <cstring>

#include "CM-inl.hpp"
#include "X86Binary.hpp"
#include "Wav16.hpp"

static const bool kTestFilter = false;
static const size_t kSizePad = 16; // 2x uint64_t: filter_size + payload_len

Archive::Header::Header()
{
  memcpy(magic_, getMagic(), kMagicStringLength);
}

void Archive::Header::read(Stream *stream)
{
  stream->read(reinterpret_cast<uint8_t *>(magic_), kMagicStringLength);
  major_version_ = stream->get16();
  minor_version_ = stream->get16();
}

void Archive::Header::write(Stream *stream)
{
  stream->write(reinterpret_cast<uint8_t *>(magic_), kMagicStringLength);
  stream->put16(major_version_);
  stream->put16(minor_version_);
}

bool Archive::Header::isArchive() const
{
  return memcmp(magic_, getMagic(), kMagicStringLength) == 0;
}

bool Archive::Header::isSameVersion() const
{
  return major_version_ == kCurMajorVersion && minor_version_ == kCurMinorVersion;
}

// ---------------------------------------------------------------------------
// splitBlock
// ---------------------------------------------------------------------------
// Splits one SolidBlock into n_parts contiguous sub-blocks of roughly equal
// uncompressed size. Each sub-block carries the same Algorithm so the
// decompressor needs no changes.
static std::vector<std::unique_ptr<Archive::SolidBlock>>
splitBlock(const Archive::SolidBlock &src, size_t n_parts)
{
  static const uint64_t kMinSplitBytes = 4ULL * 1024 * 1024;

  if (n_parts <= 1 || src.total_size_ < kMinSplitBytes)
  {
    std::vector<std::unique_ptr<Archive::SolidBlock>> single;
    single.push_back(std::unique_ptr<Archive::SolidBlock>(new Archive::SolidBlock(src.algorithm_)));
    single.back()->segments_ = src.segments_;
    single.back()->total_size_ = src.total_size_;
    return single;
  }

  const uint64_t target = (src.total_size_ + n_parts - 1) / n_parts;
  std::vector<std::unique_ptr<Archive::SolidBlock>> parts;
  parts.push_back(std::unique_ptr<Archive::SolidBlock>(new Archive::SolidBlock(src.algorithm_)));
  uint64_t part_bytes = 0;

  for (const auto &fseg : src.segments_)
  {
    size_t range_cursor = 0;
    uint64_t range_offset_within = 0;

    while (range_cursor < fseg.ranges_.size())
    {
      const auto &r = fseg.ranges_[range_cursor];
      uint64_t remaining_in_range = r.length_ - range_offset_within;

      const bool last_part = (parts.size() == n_parts);
      uint64_t space_in_part = last_part ? remaining_in_range : (target - part_bytes);
      uint64_t take = std::min(remaining_in_range, space_in_part);

      auto &cur_part = *parts.back();
      if (cur_part.segments_.empty() ||
          cur_part.segments_.back().stream_idx_ != fseg.stream_idx_)
      {
        FileSegmentStream::FileSegments new_fseg;
        new_fseg.stream_idx_ = fseg.stream_idx_;
        new_fseg.base_offset_ = fseg.base_offset_;
        new_fseg.total_size_ = 0;
        cur_part.segments_.push_back(new_fseg);
      }

      FileSegmentStream::SegmentRange new_range;
      new_range.offset_ = r.offset_ + range_offset_within;
      new_range.length_ = take;
      cur_part.segments_.back().ranges_.push_back(new_range);
      cur_part.segments_.back().total_size_ += take;
      cur_part.total_size_ += take;
      part_bytes += take;
      range_offset_within += take;

      if (range_offset_within >= r.length_)
      {
        ++range_cursor;
        range_offset_within = 0;
      }

      if (!last_part && part_bytes >= target)
      {
        part_bytes = 0;
        parts.push_back(std::unique_ptr<Archive::SolidBlock>(new Archive::SolidBlock(src.algorithm_)));
      }
    }
  }

  while (parts.size() > 1 && parts.back()->total_size_ == 0)
    parts.pop_back();

  uint64_t check_sum = 0;
  for (const auto &p : parts)
    check_sum += p->total_size_;
  check(check_sum == src.total_size_);

  return parts;
}

Archive::Algorithm::Algorithm(const CompressionOptions &options, Detector::Profile profile) : profile_(profile)
{
  mem_usage_ = options.mem_usage_;
  algorithm_ = Compressor::kTypeStore;
  filter_ = FilterType::kFilterTypeNone;

  if (profile == Detector::kProfileWave16)
  {
    algorithm_ = Compressor::kTypeWav16;
    // algorithm_ = Compressor::kTypeStore;
  }
  else
  {
    switch (options.comp_level_)
    {
    case kCompLevelStore:
      algorithm_ = Compressor::kTypeStore;
      break;
    case kCompLevelZip:
      algorithm_ = Compressor::kTypeCMZip;
      break;
    case kCompLevelTurbo:
      algorithm_ = Compressor::kTypeCMTurbo;
      break;
    case kCompLevelFast:
      algorithm_ = Compressor::kTypeCMFast;
      break;
    case kCompLevelMid:
      algorithm_ = Compressor::kTypeCMMid;
      break;
    case kCompLevelHigh:
      algorithm_ = Compressor::kTypeCMHigh;
      break;
    case kCompLevelMax:
      algorithm_ = Compressor::kTypeCMMax;
      break;
    case kCompLevelSimple:
      algorithm_ = Compressor::kTypeCMSimple;
      break;
    }
  }
  switch (profile)
  {
  case Detector::kProfileBinary:
    lzp_enabled_ = true;
    filter_ = kFilterTypeX86;
    break;
  case Detector::kProfileText:
    lzp_enabled_ = true;
    filter_ = kFilterTypeDict;
    break;
  }
  // Overrrides.
  if (options.lzp_type_ == kLZPTypeEnable)
    lzp_enabled_ = true;
  else if (options.lzp_type_ == kLZPTypeDisable)
    lzp_enabled_ = false;
  // Force filter.
  if (options.filter_type_ != kFilterTypeAuto)
  {
    filter_ = options.filter_type_;
  }
}

Archive::Algorithm::Algorithm(Stream *stream)
{
  read(stream);
}

void Archive::init()
{
  opt_var_ = 0;
}

Archive::Archive(Stream *stream, const CompressionOptions &options) : stream_(stream), options_(options)
{
  init();
  header_.write(stream_);
}

Archive::Archive(Stream *stream) : stream_(stream)
{
  init();
  header_.read(stream_);
}

Compressor *Archive::Algorithm::CreateCompressor(const FrequencyCounter<256> &freq)
{
  switch (algorithm_)
  {
  case Compressor::kTypeStore:
    return new Store;
  case Compressor::kTypeWav16:
    return new Wav16;
  case Compressor::kTypeCMZip:
    return new cm::CM<2, false>(freq, mem_usage_, lzp_enabled_, profile_);
  case Compressor::kTypeCMTurbo:
    return new cm::CM<3, /*sse*/ false>(freq, mem_usage_, lzp_enabled_, profile_);
  case Compressor::kTypeCMFast:
    return new cm::CM<4, /*sse*/ false>(freq, mem_usage_, lzp_enabled_, profile_);
  case Compressor::kTypeCMMid:
    return new cm::CM<6, /*sse*/ false>(freq, mem_usage_, lzp_enabled_, profile_);
  case Compressor::kTypeCMHigh:
    return new cm::CM<10, /*sse*/ false>(freq, mem_usage_, lzp_enabled_, profile_);
  case Compressor::kTypeCMMax:
    return new cm::CM<13, /*sse*/ true>(freq, mem_usage_, lzp_enabled_, profile_);
  case Compressor::kTypeCMSimple:
    return new cm::CM<6, false>(freq, mem_usage_, lzp_enabled_, Detector::kProfileSimple);
  }
  return nullptr;
}

void Archive::Algorithm::read(Stream *stream)
{
  mem_usage_ = static_cast<uint8_t>(stream->get());
  algorithm_ = static_cast<Compressor::Type>(stream->get());
  lzp_enabled_ = stream->get() != 0;
  filter_ = static_cast<FilterType>(stream->get());
  profile_ = static_cast<Detector::Profile>(stream->get());
}

void Archive::Algorithm::write(Stream *stream)
{
  stream->put(mem_usage_);
  stream->put(algorithm_);
  stream->put(lzp_enabled_);
  stream->put(filter_);
  stream->put(profile_);
}

std::ostream &operator<<(std::ostream &os, CompLevel comp_level)
{
  switch (comp_level)
  {
  case kCompLevelStore:
    return os << "store";
  case kCompLevelTurbo:
    return os << "turbo";
  case kCompLevelFast:
    return os << "fast";
  case kCompLevelMid:
    return os << "mid";
  case kCompLevelHigh:
    return os << "high";
  case kCompLevelMax:
    return os << "max";
  case kCompLevelSimple:
    return os << "simple";
  }
  return os << "unknown";
}

Filter *Archive::Algorithm::createFilter(Stream *stream, Analyzer *analyzer, Archive &archive, size_t opt_var,
                                         const Dict::CodeWordSet *prebuilt_code_words)
{
  Filter *ret = nullptr;
  switch (filter_)
  {
  case kFilterTypeDict:
    if (analyzer != nullptr)
    {
      auto &builder = analyzer->getDictBuilder();
      Dict::CodeWordGeneratorFast generator;
      Dict::CodeWordSet code_words;
      auto dict_filter = new Dict::Filter(stream, 0x3, 0x4, 0x6);
      const auto &dict_file = archive.Options().dict_file_;
      if (!dict_file.empty())
      {
        std::ifstream fin(dict_file.c_str(), std::ios_base::in);
        int count = 0;
        fin >> count >> code_words.num1_ >> code_words.num2_ >> code_words.num3_;
        if (count > 0 && count < 10000000)
        {
          std::string line;
          while (std::getline(fin, line))
          {
            if (!line.empty())
            {
              WordCount count(line);
              code_words.GetCodeWords()->push_back(count);
            }
          }
          auto temp = code_words.codewords_;
          if (true)
          {
            Permute(&code_words.codewords_[0], &temp[0], archive.opt_vars_, code_words.num1_);
          }
          else
          {
            uint8_t perm0[] = {
                2,
                3,
                29,
                4,
                22,
                10,
                8,
                7,
                9,
                11,
                12,
                18,
                13,
                14,
                1,
                5,
                17,
                23,
                21,
                0,
                24,
                25,
                26,
                20,
                19,
                6,
                27,
                16,
                15,
                28,
                30,
                31,
                32,
                33,
                35,
                34,
                37,
                36,
                38,
                39,
            };
            Permute(&code_words.codewords_[0], &temp[0], perm0, code_words.num1_);
          }
          auto count2 = code_words.num2_ * 128;
          if (false)
            if (false)
            {
              Permute(&code_words.codewords_[code_words.num1_], &temp[code_words.num1_], archive.opt_vars_, count2);
            }
            else
            {
              auto opts = ReadCSI<size_t>("optin.txt");
              check(opts.size() == count2);
              Permute(&code_words.codewords_[code_words.num1_], &temp[code_words.num1_], &opts[0], count2);
            }
          if (code_words.num1_ == 0 && code_words.num2_ == 0 && code_words.num3_ == 0)
          {
            code_words.num1_ = 32 + 5;
            code_words.num2_ = 128 - code_words.num1_;
            code_words.num3_ = 128 - code_words.num1_ - code_words.num2_;
            auto remain = count - code_words.num1_;
            while (code_words.num2_ > 0 &&
                   code_words.num3_ < 128 - code_words.num1_ &&
                   code_words.num2_ * 128 + code_words.num3_ * 128 * 128 < remain)
            {
              code_words.num2_--;
              code_words.num3_++;
            }
          }
          std::cerr << "Number of words for dictionary " << count << " " << code_words.num1_ << " " << code_words.num2_ << " " << code_words.num3_ << std::endl;
        }
        else
        {
          std::cerr << "Invalid number of words for dictionary " << count << std::endl;
        }
      }
      CodeWordMap dict_codes;
      // dict_codes.Add(0, 32);
      dict_codes.Add(128, 256);
      size_t num_code_bytes = 128 + 0;
      // size_t num_code_bytes = 128;
      if (prebuilt_code_words != nullptr)
      {
        // Shared pre-built dict (parallel split blocks) -- use directly, do not drain builder.
        code_words = *prebuilt_code_words;
      }
      else if (code_words.GetCodeWords()->empty())
      {
        generator.Generate(builder, &code_words, 5, 40, 32, dict_codes.Count());
      }
      const auto &out_dict_file = archive.Options().out_dict_file_;
      if (!out_dict_file.empty())
      {
        std::ofstream fout(out_dict_file.c_str());
        fout << code_words.codewords_.size() << " " << code_words.num1_ << " " << code_words.num2_ << " " << code_words.num3_ << std::endl;
        for (const auto &s : code_words.codewords_)
        {
          fout << s.Word() << std::endl;
        }
      }
      auto &freq = builder.FrequencyCounter();
      dict_filter->AddCodeWords(code_words.GetCodeWords(), code_words.num1_, code_words.num2_, code_words.num3_, &freq, dict_codes.Count());
      if (false)
      {
        std::cerr << std::endl
                  << "Before " << freq.Sum() << std::endl;
        auto *tree = Huffman::Tree<uint32_t>::BuildPackageMerge(freq.GetFrequencies(), 256, 16);
        Huffman::Code codes[256];
        tree->GetCodes(codes);
        uint64_t total_bits = 0;
        for (size_t i = 0; i < 256; ++i)
        {
          std::cerr << i << " bits " << codes[i].length << " freq " << freq.GetFrequencies()[i] << std::endl;
          total_bits += codes[i].length * freq.GetFrequencies()[i];
        }
        std::cerr << std::endl
                  << "After " << freq.Sum() << " huff " << total_bits / kBitsPerByte << std::endl;
      }
      dict_filter->SetFrequencies(freq);
      dict_filter->setOpt(opt_var);
      ret = dict_filter;
    }
    else
    {
      ret = new Dict::Filter(stream);
    }
    break;
  case kFilterTypeX86:
    ret = new X86AdvancedFilter(stream);
    break;
  }
  if (ret != nullptr)
  {
    ret->setOpt(opt_var);
  }
  return ret;
}

void Archive::constructBlocks(Analyzer::Blocks *blocks_for_file)
{
}

Compressor *Archive::createMetaDataCompressor()
{
  if (kIsDebugBuild)
  {
    return new Store;
  }
  return new cm::CM<6, false>(FrequencyCounter<256>(), 6, true, Detector::kProfileText);
}

void Archive::writeBlocks()
{
  std::vector<uint8_t> temp;
  WriteVectorStream wvs(&temp);
  // Write out the blocks into temp.
  blocks_.write(&wvs);
  size_t blocks_size = wvs.tell();
  files_.write(&wvs);
  size_t files_size = wvs.tell() - blocks_size;
  // Compress overhead.
  std::unique_ptr<Compressor> c(createMetaDataCompressor());
  c->setOpt(opt_var_);
  c->setOpts(opt_vars_);
  ReadMemoryStream rms(&temp[0], &temp[0] + temp.size());
  auto start_pos = stream_->tell();
  stream_->leb128Encode(temp.size());
  c->compress(&rms, stream_);
  stream_->leb128Encode(static_cast<uint64_t>(1234u));
  std::cout << "(flist=" << files_size << "+" << "blocks=" << blocks_size << ")=" << temp.size() << " -> " << stream_->tell() - start_pos << std::endl
            << std::endl;
}

void Archive::readBlocks()
{
  if (!files_.empty())
  {
    // Already read.
    return;
  }
  auto metadata_size = stream_->leb128Decode();
  std::cout << "Metadata size=" << metadata_size << std::endl;
  // Decompress overhead.
  std::unique_ptr<Compressor> c(createMetaDataCompressor());
  std::vector<uint8_t> metadata;
  WriteVectorStream wvs(&metadata);
  auto start_pos = stream_->tell();
  c->decompress(stream_, &wvs, metadata_size);
  auto cmp = stream_->leb128Decode();
  check(cmp == 1234u);
  ReadMemoryStream rms(&metadata);
  blocks_.read(&rms);
  files_.read(&rms);
}

void Archive::Blocks::write(Stream *stream)
{
  stream->leb128Encode(size());
  for (auto &block : *this)
  {
    block->write(stream);
  }
}

void Archive::Blocks::read(Stream *stream)
{
  size_t num_blocks = stream->leb128Decode();
  check(num_blocks < 1000000); // Sanity check.
  clear();
  for (size_t i = 0; i < num_blocks; ++i)
  {
    std::unique_ptr<SolidBlock> block(new SolidBlock);
    block->read(stream);
    push_back(std::move(block));
  }
}

void Archive::SolidBlock::write(Stream *stream)
{
  algorithm_.write(stream);
  stream->leb128Encode(segments_.size());
  for (auto &seg : segments_)
  {
    seg.write(stream);
  }
}

void Archive::SolidBlock::read(Stream *stream)
{
  algorithm_.read(stream);
  size_t num_segments = stream->leb128Decode();
  check(num_segments < 10000000);
  segments_.resize(num_segments);
  total_size_ = 0;
  for (auto &seg : segments_)
  {
    seg.read(stream);
    seg.calculateTotalSize();
    total_size_ += seg.total_size_;
  }
}

class FileSegmentStreamFileList : public FileSegmentStream
{
public:
  FileSegmentStreamFileList(std::vector<FileSegments> *segments, uint64_t count, FileList *file_list, bool extract, bool verify)
      : FileSegmentStream(segments, count), file_list_(file_list), extract_(extract), verify_(verify) {}
  ~FileSegmentStreamFileList()
  {
    // Open remaining streams if zero sized?
    if (extract_ && !verify_)
    {
      size_t index = 0;
      for (auto &file_info : *file_list_)
      {
        if (!file_info.isDir() && !file_info.previouslyOpened())
        {
          openNewStream(index);
        }
        ++index;
      }
    }
    delete cur_stream_;
  }
  Stream *openNewStream(size_t index) OVERRIDE
  {
    if (cur_stream_ != nullptr)
    {
      delete cur_stream_;
      cur_stream_ = nullptr;
    }
    // Open the new file.
    std::unique_ptr<File> ret(new File);
    auto &file_info = file_list_->at(index);
    std::string full_name = file_info.getFullName();
    int err;
    if (extract_)
    {
      std::ios_base::openmode open_mode = std::ios_base::out | std::ios_base::binary;
      if (file_info.previouslyOpened())
      {
        open_mode |= std::ios_base::in;
      }
      file_info.addOpen();
      err = ret->open(full_name.c_str(), open_mode);
    }
    else
    {
      err = ret->open(full_name.c_str(), std::ios_base::in | std::ios_base::binary);
    }
    if (err != 0)
    {
      std::cerr << "Error opening: " << full_name.c_str() << " " << err << "(" << errstr(err) << ")" << " code " << std::endl;
    }
    return ret.release();
  }

private:
  FileList *const file_list_;
  const bool extract_;
  const bool verify_;
};

class VerifyFileSegmentStreamFileList : public FileSegmentStream
{
public:
  VerifyFileSegmentStreamFileList(std::vector<FileSegments> *segments, FileList *file_list, std::vector<uint64_t> *remain_bytes)
      : FileSegmentStream(segments, 0u), file_list_(file_list), verify_stream_(&file_, 0), remain_bytes_(remain_bytes)
  {
  }
  ~VerifyFileSegmentStreamFileList()
  {
    subBytes(last_idx_);
  }
  void subBytes(size_t idx)
  {
    auto c = verify_stream_.getCount();
    if (c == 0)
    {
      return;
    }
    auto &r = remain_bytes_->at(idx);
    if (c > r)
    {
      std::cerr << "Wrote " << c - r << " extra bytes to " << file_list_->at(idx).getFullName() << std::endl;
      r = 0;
    }
    else
    {
      r -= c;
    }
  }
  Stream *openNewStream(size_t index) OVERRIDE
  {
    subBytes(last_idx_);
    verify_stream_.resetCount();
    // Open the new file.
    auto &file_info = file_list_->at(last_idx_ = index);
    std::string full_name = file_info.getFullName();
    if (int err = file_.open(full_name.c_str(), std::ios_base::in | std::ios_base::binary))
    {
      std::cerr << "Error opening: " << full_name.c_str() << " (" << errstr(err) << ")" << std::endl;
    }
    return &verify_stream_;
  }
  uint64_t totalDifferences() const
  {
    return verify_stream_.differences_;
  }

private:
  FileList *const file_list_;
  File file_;
  VerifyStream verify_stream_;
  std::vector<uint64_t> *const remain_bytes_;
  size_t last_idx_ = 0;
};

void testFilter(Stream *stream, Analyzer *analyzer)
{
  std::vector<uint8_t> comp;
  stream->seek(0);
  auto start = clock();
  {
    auto &builder = analyzer->getDictBuilder();
    Dict::CodeWordGeneratorFast generator;
    Dict::CodeWordSet code_words;
    generator.Generate(builder, &code_words, 8);
    auto dict_filter = new Dict::Filter(stream, 0x3, 0x4, 0x6);
    dict_filter->AddCodeWords(code_words.GetCodeWords(), code_words.num1_, code_words.num2_, code_words.num3_, nullptr);
    WriteVectorStream wvs(&comp);
    Store store;
    store.compress(dict_filter, &wvs, std::numeric_limits<uint64_t>::max());
  }
  uint64_t size = stream->tell();
  std::cout << "Filter comp " << size << " -> " << comp.size() << " in " << clockToSeconds(clock() - start) << "s" << std::endl;
  // Test revser speed
  start = clock();
  stream->seek(0);
  VoidWriteStream voids;
  {
    ReadMemoryStream rms(&comp);
    Store store;
    Dict::Filter filter_out(&voids);
    store.decompress(&rms, &filter_out, size);
    filter_out.flush();
  }
  std::cout << "Void decomp " << voids.tell() << " <- " << comp.size() << " in " << clockToSeconds(clock() - start) << "s" << std::endl;
  // Test reverse.
  start = clock();
  stream->seek(0);
  VerifyStream vs(stream, size);
  {
    ReadMemoryStream rms(&comp);
    Store store;
    Dict::Filter filter_out(&vs);
    store.decompress(&rms, &filter_out, size);
    filter_out.flush();
    vs.summary();
  }
  std::cout << "Verify decomp " << vs.tell() << " <- " << comp.size() << " in " << clockToSeconds(clock() - start) << "s" << std::endl
            << std::endl;
}

static inline std::string smartExt(const std::string &ext)
{
  if (ext == "h" || ext == "hpp" || ext == "inl" || ext == "cpp")
    return "c";
  if (ext == "jpg" || ext == "zip" || ext == "7z" || ext == "apk" || ext == "mp3" || ext == "gif" || ext == "png")
    return "�" + ext;
  return ext;
}

class CompareFileInfoName
{
public:
  bool operator()(const FileInfo &a, const FileInfo &b) const
  {
    if (a.isDir() != b.isDir())
    {
      return a.isDir() > b.isDir();
    }
    if (a.isDir())
    {
      return a.getFullName() < b.getFullName();
    }
    auto &name1 = a.getName();
    auto &name2 = b.getName();
    auto ext1 = getExt(name1);
    auto ext2 = getExt(name2);
    auto sext1 = smartExt(ext1);
    auto sext2 = smartExt(ext2);
    if (sext1 != sext2)
      return sext1 < sext2;
    auto fname1 = GetFileName(name1).second;
    auto fname2 = GetFileName(name2).second;
    if (false)
    {
      // Probably buggy.
      if (!ext1.empty())
        fname1 = fname1.substr(0, fname1.length() - ext1.length() - 1);
      if (!ext2.empty())
        fname2 = fname2.substr(0, fname2.length() - ext2.length() - 1);
      if (isdigit(fname1.back()) && isdigit(fname2.back()))
      {
        size_t d1 = fname1.length() - 1;
        for (; d1 > 0 && isdigit(fname1[d1]); --d1)
          ;
        size_t d2 = fname2.length() - 1;
        for (; d2 > 0 && isdigit(fname2[d2]); --d2)
          ;
        auto no_num1 = fname1.substr(0, d1 + 1);
        auto no_num2 = fname2.substr(0, d2 + 1);
        if (no_num1 != no_num2)
          return no_num1 < no_num2;
        auto num1 = fname1.substr(d1 + 1);
        auto num2 = fname2.substr(d2 + 1);
        auto l1 = num1.length();
        auto l2 = num2.length();
        if (l1 > l2)
          num2 = std::string(l1 - l2, '0') + num2;
        else if (l1 < l2)
          num1 = std::string(l2 - l1, '0') + num1;
        if (num1 != num2)
          return num1 < num2;
      }
    }
    if (fname1 != fname2)
      return fname1 < fname2;
    return name1 < name2;
  }
};

class AnalyzerProgressThread : public AutoUpdater
{
public:
  AnalyzerProgressThread() : stream_(nullptr), start_(clock()), add_bytes_(0), add_files_(0)
  {
  }

  void setStream(Stream *stream)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    stream_ = stream;
  }

  void doneFile(uint64_t file_bytes)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    stream_ = 0;
    add_bytes_ += file_bytes;
    ++add_files_;
  }
  virtual void print()
  {
    auto cur_time = clock();
    auto time_delta = cur_time - start_;
    if (!time_delta)
      ++time_delta;
    const uint64_t cur_bytes = (stream_ != nullptr ? stream_->tell() : 0u) + add_bytes_;
    const uint32_t rate = uint32_t(double(cur_bytes / KB) / (double(time_delta) / double(CLOCKS_PER_SEC)));
    std::cout << "Analyzed " << add_files_ << " size=" << prettySize(cur_bytes) << " " << rate << "KB/s   ";
    std::cout << "\t\r" << std::flush;
  }

private:
  Stream *stream_;
  size_t start_;
  uint64_t add_bytes_;
  uint64_t add_files_;
};

struct DedupeFragment
{
  uint32_t src_file_;
  uint64_t src_pos_;
  uint32_t dest_file_;
  uint64_t dest_pos_;
  uint64_t len_;
};

class DedupeAnalyzer : public Analyzer
{
  static const size_t kBlockSize = 8 * KB;

public:
  DedupeAnalyzer(FileList *files) : files_(files)
  {
  }
  std::pair<uint64_t, uint64_t> confirmDedupe(Deduplicator::DedupEntry *e, Stream *stream, size_t file_idx, uint64_t pos)
  {
    uint8_t file_block[kBlockSize];
    uint8_t compare_block[kBlockSize];
    uint64_t file_pos = e->offset_;
    uint64_t compare_pos = pos;
    Stream *file_stream;
    File file;
    auto orig_pos = stream->tell();
    uint64_t max_read = std::numeric_limits<uint64_t>::max();
    if (e->file_idx_ == file_idx)
    {
      if (file_pos >= compare_pos)
      {
        return std::pair<uint64_t, uint64_t>(0u, 0u);
      }
      max_read = compare_pos - file_pos;
      file_stream = stream;
    }
    else
    {
      auto &file_info = files_->at(e->file_idx_);
      int err;
      std::string file_name = file_info.getFullName();
      if (err = file.open(file_name.c_str(), std::ios_base::in | std::ios_base::binary))
      {
        std::cerr << "Error opening: " << file_name << " (" << errstr(err) << ")" << std::endl;
      }
      file_stream = &file;
    }
    // Extend the match.
    uint64_t len = 0;
    for (;;)
    {
      size_t cur_max = static_cast<size_t>(std::min(kBlockSize, max_read - len));
      auto c1 = stream->readat(compare_pos + len, compare_block, cur_max);
      auto c2 = file_stream->readat(file_pos + len, file_block, cur_max);
      if (c1 == 0 || c2 == 0)
        break;
      size_t cur_len;
      bool match = true;
      for (cur_len = 0; match && cur_len < c1 && cur_len < c2; ++cur_len)
      {
        match = compare_block[cur_len] == file_block[cur_len];
      }
      len += cur_len;
      if (!match)
        break;
    }
    // Extend backwards.
    for (;;)
    {
      // TODO: This is not correct.
      auto pos0 = compare_pos >= kBlockSize ? compare_pos - kBlockSize : 0u;
      auto pos1 = file_pos >= kBlockSize ? file_pos - kBlockSize : 0u;
      auto c1 = stream->readat(pos0, compare_block, compare_pos - pos0);
      auto c2 = file_stream->readat(pos1, file_block, file_pos - pos1);
      size_t cur_len = 0;
      for (; c1 != 0 && c2 != 0; ++cur_len)
      {
        if (compare_block[c1 - 1] != file_block[c2 - 1])
        {
          break;
        }
        --c1;
        --c2;
      }
      if (cur_len == 0)
        break;
      compare_pos -= cur_len;
      file_pos -= cur_len;
      len += cur_len;
    }
    // Back to where we started.
    stream->seek(orig_pos);
    file.close();
    if (len < 1024)
    {
      return std::pair<uint64_t, uint64_t>(0u, 0u);
    }
    // Write the dedupe fragment.
    DedupeFragment frag;
    frag.src_file_ = e->file_idx_;
    frag.src_pos_ = file_pos;
    frag.dest_file_ = file_idx;
    frag.dest_pos_ = compare_pos;
    frag.len_ = len;
    dedupe_fragments_.push_back(frag);
    return std::pair<uint64_t, uint64_t>(compare_pos, len);
  }
  void dump()
  {
    uint64_t total_dedupe = 0;
    for (auto &f : dedupe_fragments_)
    {
      const auto &file1 = files_->at(f.src_file_);
      const auto &file2 = files_->at(f.dest_file_);
      std::cout << f.len_ << ":" << file1.getName() << "(" << f.src_pos_ << ")->" << file2.getName() << "(" << f.dest_pos_ << ")" << std::endl;
      total_dedupe += f.len_;
    }
    std::cout << "Total dedupe " << prettySize(total_dedupe) << std::endl;
  }

private:
  FileList *files_;
  std::vector<DedupeFragment> dedupe_fragments_;
};

uint64_t Archive::compress(const std::vector<FileInfo> &in_files)
{
  std::list<std::string> prefixes;
  blocks_.clear();
  // Enumerate files
  auto start = clock();
  std::cout << "Enumerating files" << std::endl;
  for (auto f : in_files)
  {
    const std::string cur_name(f.getName());
    const bool absolute_path = IsAbsolutePath(cur_name);
    if (absolute_path)
    {
      auto pair = GetFileName(cur_name);
      prefixes.push_back(pair.first);
      f.setPrefix(&prefixes.back());
      f.SetName(pair.second);
    }
    files_.push_back(f);
    // If abslute, take prefix directory as prefix.
    if (f.isDir())
    {
      if (absolute_path)
      {
        auto pair = GetFileName(cur_name);
        prefixes.push_back(pair.first);
        files_.addDirectoryRec(pair.second, &prefixes.back());
      }
      else
      {
        files_.addDirectoryRec(f.getName());
      }
    }
  }
  std::sort(files_.begin(), files_.end(), CompareFileInfoName());
  std::cout << "Enumerating took " << clockToSeconds(clock() - start) << "s" << std::endl;

  for (size_t i = 0; i < Detector::kProfileCount; ++i)
  {
    Algorithm a(options_, static_cast<Detector::Profile>(i));
    blocks_.push_back(std::unique_ptr<SolidBlock>(new SolidBlock(a)));
  }
  Analyzer analyzer;
  {
    // Analyze enumerated and construct blocks.
    analyzer.setOpt(opt_var_);
    start = clock();
    std::cout << "Analyzing " << files_.size() << " files" << std::endl;
    size_t file_idx = 0;
    uint64_t total_size = 0;
    AnalyzerProgressThread thr;
    for (auto &f : files_)
    {
      if (!f.isDir())
      {
        File fin;
        int err;
        if (err = fin.open(f.getFullName(), std::ios_base::in | std::ios_base::binary))
        {
          std::cerr << "Error opening: " << f.getName() << " (" << errstr(err) << ")" << std::endl;
        }
        thr.setStream(&fin);
        analyzer.analyze(&fin, file_idx);
        auto &blocks = analyzer.getBlocks();
        if (blocks.empty())
        {
          blocks.push_back(Detector::DetectedBlock());
        }
        uint64_t pos = 0;
        for (const auto &block : blocks_)
        {
          // Compress each stream type.
          pos = 0;
          FileSegmentStream::FileSegments seg;
          seg.base_offset_ = 0;
          seg.stream_idx_ = file_idx;
          for (const auto &b : blocks)
          {
            const auto len = b.length();
            if (b.profile() == block->algorithm_.profile())
            {
              FileSegmentStream::SegmentRange range{pos, len};
              seg.ranges_.push_back(range);
            }
            pos += len;
          }
          seg.calculateTotalSize();
          if (!seg.ranges_.empty())
          {
            block->segments_.push_back(seg);
            block->total_size_ += seg.total_size_;
          }
        }
        thr.doneFile(pos);
        total_size += pos;
        blocks.clear();
      }
      file_idx++;
    }
    std::cout << std::endl;
    analyzer.dump();
    std::cout << "Analyzing took " << clockToSeconds(clock() - start) << "s" << std::endl
              << std::endl;
  }
  // Remove empty blocks.
  auto it = std::remove_if(blocks_.begin(), blocks_.end(), [](const std::unique_ptr<SolidBlock> &b)
                           { return b->total_size_ == 0; });
  blocks_.erase(it, blocks_.end());
  for (const auto &b : blocks_)
    check(b->total_size_ > 0);
  // Biggest block first (decompression performance reasons).
  std::sort(blocks_.rbegin(), blocks_.rend(), [](const std::unique_ptr<SolidBlock> &a, const std::unique_ptr<SolidBlock> &b)
            { return a->total_size_ < b->total_size_; });
  // Cap threads for high memory levels to avoid OOM.
  // CM<10> and CM<13> need ~360-720MB per thread at mem=7.
  {
    const auto lvl = options_.comp_level_;
    if ((lvl == kCompLevelMax || lvl == kCompLevelHigh) && num_threads_ > 1)
    {
      std::cout << "Note: capping threads to 1 for high memory level." << std::endl;
      num_threads_ = 1;
    }
  }

  // Split large blocks so all threads stay busy.
  // A single huge block (e.g. 95% of total) would leave all other threads
  // idle. Split any block larger than (total / num_threads) into sub-blocks.
  if (num_threads_ > 1)
  {
    uint64_t grand_total = 0;
    for (const auto &b : blocks_)
      grand_total += b->total_size_;
    const uint64_t per_thread = (grand_total + num_threads_ - 1) / num_threads_;
    static const uint64_t kMinSplit = 4ULL * 1024 * 1024;

    Blocks expanded;
    for (auto &bptr : blocks_)
    {
      if (bptr->total_size_ > per_thread && bptr->total_size_ >= kMinSplit)
      {
        size_t n_parts = static_cast<size_t>((bptr->total_size_ + per_thread - 1) / per_thread);
        if (n_parts > num_threads_)
          n_parts = num_threads_;
        std::cout << "Splitting " << Detector::profileToString(bptr->algorithm_.profile())
                  << " block (" << formatNumber(bptr->total_size_) << ")"
                  << " into " << n_parts << " parts of ~"
                  << formatNumber(bptr->total_size_ / n_parts) << " each" << std::endl;
        auto parts = splitBlock(*bptr, n_parts);
        for (auto &p : parts)
          expanded.push_back(std::move(p));
      }
      else
      {
        expanded.push_back(std::move(bptr));
      }
    }
    blocks_ = std::move(expanded);
    std::cout << std::endl;
  }

  // Pre-build shared dict for text blocks when using multiple threads.
  // Each thread would otherwise drain the builder WordCounter independently,
  // producing a degraded per-thread dictionary.  Generate once, share read-only.
  std::unique_ptr<Dict::CodeWordSet> shared_text_dict;
  {
    bool has_text_block = false;
    for (const auto &b : blocks_)
    {
      if (b->algorithm_.profile() == Detector::kProfileText)
      {
        has_text_block = true;
        break;
      }
    }
    if (has_text_block && num_threads_ > 1)
    {
      auto &builder = analyzer.getDictBuilder();
      shared_text_dict.reset(new Dict::CodeWordSet());
      Dict::CodeWordGeneratorFast gen;
      CodeWordMap dict_codes;
      dict_codes.Add(128, 256);
      gen.Generate(builder, shared_text_dict.get(), 5, 40, 32, dict_codes.Count());
      std::cout << "Pre-built shared dict: " << shared_text_dict->codewords_.size() << " words" << std::endl;
    }
  }

  writeBlocks();

  // Parallel compress + serial merge.
  // Each block compressed to in-memory buffer by a worker thread.
  // Results merged to stream_ in deterministic order.
  struct BlockResult
  {
    std::vector<uint8_t> data;
    uint64_t filter_size;
    uint64_t uncompressed_size;
    double elapsed;
    std::string profile_name;
  };

  auto compressBlock = [&](const SolidBlock *block, const Dict::CodeWordSet *prebuilt) -> BlockResult
  {
    try
    {
      BlockResult result;
      result.profile_name = Detector::profileToString(block->algorithm_.profile());
      result.data.reserve(block->total_size_ + kSizePad);
      WriteVectorStream wvs(&result.data);
      for (size_t i = 0; i < kSizePad; ++i)
        wvs.put(0);
      const clock_t t0 = clock();
      FileSegmentStreamFileList segstream(
          const_cast<std::vector<FileSegmentStream::FileSegments> *>(&block->segments_),
          0, &files_, false, false);
      Algorithm algo_copy = block->algorithm_;
      std::unique_ptr<Filter> filter(algo_copy.createFilter(&segstream, &analyzer, *this, opt_var_, prebuilt));
      Stream *in_stream = &segstream;
      FrequencyCounter<256> freq;
      if (filter != nullptr)
      {
        in_stream = filter.get();
        freq = filter->GetFrequencies();
      }
      auto in_start = in_stream->tell();
      std::unique_ptr<Compressor> comp(algo_copy.CreateCompressor(freq));
      comp->setOpt(opt_var_);
      comp->setOpts(opt_vars_);
      comp->compress(in_stream, &wvs);
      result.filter_size = in_stream->tell() - in_start;
      result.uncompressed_size = segstream.tell();
      result.elapsed = clockToSeconds(clock() - t0);
      return result;
    }
    catch (const std::exception &e)
    {
      std::cerr << "COMPRESS EXCEPTION: " << e.what() << std::endl
                << std::flush;
      throw;
    }
    catch (...)
    {
      std::cerr << "COMPRESS UNKNOWN EXCEPTION" << std::endl
                << std::flush;
      throw;
    }
  };

  const size_t num_blocks = blocks_.size();
  std::vector<std::future<BlockResult>> futures(num_blocks);
  size_t next_launch = 0;
  size_t next_merge = 0;
  uint64_t total = 0;

  std::cout << "Compressing " << num_blocks << " block(s) using up to "
            << num_threads_ << " thread(s)" << std::endl
            << std::endl;

  // Pre-fill sliding window with async tasks.
  while (next_launch < num_blocks && next_launch < num_threads_)
  {
    const SolidBlock *block = blocks_[next_launch].get();
    const Dict::CodeWordSet *prebuilt =
        (shared_text_dict && block->algorithm_.profile() == Detector::kProfileText)
            ? shared_text_dict.get()
            : nullptr;
    futures[next_launch] = std::async(std::launch::async, compressBlock, block, prebuilt);
    ++next_launch;
  }

  while (next_merge < num_blocks)
  {
    BlockResult result = [&]() -> BlockResult
    {
      try
      {
        return futures[next_merge].get();
      }
      catch (const std::exception &e)
      {
        std::cerr << "ERROR compressing block " << next_merge << ": " << e.what() << std::endl;
        throw;
      }
    }();

    // Launch next block immediately.
    if (next_launch < num_blocks)
    {
      const SolidBlock *block = blocks_[next_launch].get();
      const Dict::CodeWordSet *prebuilt =
          (shared_text_dict && block->algorithm_.profile() == Detector::kProfileText)
              ? shared_text_dict.get()
              : nullptr;
      futures[next_launch] = std::async(std::launch::async, compressBlock, block, prebuilt);
      ++next_launch;
    }

    // Write to output stream (serial, deterministic).
    auto out_start = stream_->tell();
    for (size_t i = 0; i < kSizePad; ++i)
      stream_->put(0);
    const uint8_t *payload = result.data.data() + kSizePad;
    const size_t payload_len = result.data.size() - kSizePad;
    stream_->write(const_cast<uint8_t *>(payload), payload_len);
    auto after_pos = stream_->tell();
    // Fix up header: [8 bytes filter_size LE][8 bytes payload_len LE]
    stream_->seek(out_start);
    auto write_u64le = [&](uint64_t v)
    {
      for (int i = 0; i < 8; ++i)
      {
        stream_->put(v & 0xFF);
        v >>= 8;
      }
    };
    write_u64le(static_cast<uint64_t>(result.filter_size));
    write_u64le(static_cast<uint64_t>(payload_len));
    stream_->seek(after_pos);

    std::cout << "Compressed " << formatNumber(result.uncompressed_size)
              << " -> " << formatNumber(after_pos - out_start)
              << " [" << result.profile_name << "]"
              << " in " << result.elapsed << "s" << std::endl
              << std::endl;

    check(result.uncompressed_size == blocks_[next_merge]->total_size_);
    total += result.uncompressed_size;
    ++next_merge;
  }

  files_.clear();
  return total;
}

// Decompress.
void Archive::decompress(const std::string &out_dir, bool verify)
{
  readBlocks();
  // Count non-directory files to decide single vs multi file mode.
  size_t non_dir_count = 0;
  for (const auto &f : files_)
    if (!f.isDir())
      ++non_dir_count;
  // Single file + explicit out_dir: use out_dir as exact output path.
  // Multi file: treat out_dir as directory, append separator if needed.
  const bool single_file_override = (non_dir_count == 1 && !out_dir.empty());
  std::string dir_prefix = out_dir;
  if (!out_dir.empty() && !single_file_override)
  {
    if (out_dir.back() != '/' && out_dir.back() != '\\')
      dir_prefix += "/";
  }
  for (auto &f : files_)
  {
    if (single_file_override && !f.isDir())
    {
      f.SetName(out_dir);
      f.setPrefix(nullptr);
    }
    else
    {
      f.setPrefix(dir_prefix.empty() ? nullptr : &dir_prefix);
    }
    if (f.isDir())
    {
      // Create directories first.
      FileInfo::CreateDir(f.getFullName());
    }
  }
  std::vector<uint64_t> remain_bytes;
  if (verify)
  {
    remain_bytes.resize(files_.size(), 0u);
    for (const auto &block : blocks_)
    {
      for (const auto &seg : block->segments_)
      {
        remain_bytes.at(seg.stream_idx_) += seg.total_size_;
      }
    }
  }
  if (!verify)
  {
    for (size_t i = 0; i < files_.size(); ++i)
    {
      auto &fi = files_[i];
      if (!fi.isDir())
      {
        File f;
        int err = f.open(fi.getFullName().c_str(), std::ios_base::out | std::ios_base::binary);
        f.close();
        fi.addOpen();
      }
    }
  }

  struct BlockPayload
  {
    std::vector<uint8_t> compressed;
    uint64_t uncompressed_size;
    std::string profile_name;
    const SolidBlock *block;
  };
  std::vector<BlockPayload> payloads;
  payloads.reserve(blocks_.size());
  for (const auto &block : blocks_)
  {
    const_cast<SolidBlock *>(block.get())->total_size_ = 0;
    for (const auto &seg : block->segments_)
      const_cast<SolidBlock *>(block.get())->total_size_ += seg.total_size_;
    auto out_start = stream_->tell();
    // Read header: [8 bytes filter_size LE][8 bytes payload_len LE]
    auto read_u64le = [&]() -> uint64_t
    {
      uint64_t v = 0;
      for (int i = 0; i < 8; ++i)
        v |= (static_cast<uint64_t>(stream_->get() & 0xFF) << (i * 8));
      return v;
    };
    uint64_t filter_size = read_u64le();
    uint64_t payload_len = read_u64le();
    // kSizePad=16, we already read 16 bytes exactly -- no skip needed
    BlockPayload p;
    p.block = block.get();
    p.uncompressed_size = filter_size; // passed as max_count to CM decompress
    p.profile_name = Detector::profileToString(block->algorithm_.profile());
    p.compressed.resize(payload_len);
    size_t got = stream_->read(p.compressed.data(), payload_len);
    payloads.push_back(std::move(p));
  }

  uint64_t differences = 0;
  std::mutex diff_mutex;

  auto decompressBlock = [&](const BlockPayload &p)
  {
    const SolidBlock *block = p.block;
    const clock_t t0 = clock();
    ReadMemoryStream rms(p.compressed.data(),
                         p.compressed.data() + p.compressed.size());
    FileSegmentStreamFileList segstream(
        const_cast<std::vector<FileSegmentStream::FileSegments> *>(&block->segments_),
        0u, &files_, true, verify);
    VerifyFileSegmentStreamFileList verify_segstream(
        const_cast<std::vector<FileSegmentStream::FileSegments> *>(&block->segments_),
        &files_, &remain_bytes);
    Algorithm algo_copy = block->algorithm_;
    Stream *out_stream = verify ? static_cast<Stream *>(&verify_segstream)
                                : static_cast<Stream *>(&segstream);
    Stream *filter_out_stream = out_stream;
    std::unique_ptr<Filter> filter(algo_copy.createFilter(filter_out_stream, nullptr, *this));
    FrequencyCounter<256> freq;
    if (filter != nullptr)
    {
      filter_out_stream = filter.get();
      freq = filter->GetFrequencies();
    }
    std::unique_ptr<Compressor> comp(algo_copy.CreateCompressor(freq));
    comp->setOpt(opt_var_);
    comp->setOpts(opt_vars_);
    comp->decompress(&rms, filter_out_stream, p.uncompressed_size);
    if (filter.get() != nullptr)
      filter->flush();
    if (verify)
    {
      std::unique_lock<std::mutex> lock(diff_mutex);
      differences += verify_segstream.totalDifferences();
    }
    std::cout << "Decompressed " << formatNumber(p.uncompressed_size)
              << " [" << p.profile_name << "]"
              << " in " << clockToSeconds(clock() - t0) << "s" << std::endl;
  };

  const size_t n_blocks = payloads.size();
  std::vector<std::future<void>> dfutures(n_blocks);
  size_t dnext_launch = 0, dnext_join = 0;
  const clock_t decomp_start = clock();
  std::cout << "Decompressing " << n_blocks << " block(s) using up to "
            << num_threads_ << " thread(s)" << std::endl
            << std::endl;
  while (dnext_launch < n_blocks && dnext_launch < num_threads_)
  {
    dfutures[dnext_launch] = std::async(std::launch::async, decompressBlock,
                                        std::cref(payloads[dnext_launch]));
    ++dnext_launch;
  }
  while (dnext_join < n_blocks)
  {
    try
    {
      dfutures[dnext_join].get();
    }
    catch (const std::exception &e)
    {
      std::cerr << "ERROR decompressing block " << dnext_join << ": " << e.what() << std::endl;
      throw;
    }
    if (dnext_launch < n_blocks)
    {
      dfutures[dnext_launch] = std::async(std::launch::async, decompressBlock,
                                          std::cref(payloads[dnext_launch]));
      ++dnext_launch;
    }
    ++dnext_join;
  }

  std::cout << "Done decompressing in " << clockToSeconds(clock() - decomp_start) << "s" << std::endl
            << std::endl;

  if (verify)
  {
    for (size_t i = 0; i < files_.size(); ++i)
    {
      if (remain_bytes[i] > 0)
      {
        std::cerr << "Missed writing " << remain_bytes[i] << " bytes to " << files_[i].getFullName() << std::endl;
      }
    }
    if (differences)
    {
      std::cerr << "DECOMPRESSION FAILED, " << differences << " differences" << std::endl;
    }
    else
    {
      std::cout << "No differences found" << std::endl;
    }
  }
}

void Archive::list()
{
  readBlocks();
  for (const auto &f : files_)
  {
    std::cout << FileInfo::attrToStr(f.getAttributes()) << " " << f.getName() << std::endl;
  }
  uint64_t total_size = 0, idx = 0;
  for (const auto &b : blocks_)
  {
    if (b->total_size_ > 0)
    {
      std::cout << "Solid block " << idx++ << " size " << formatNumber(b->total_size_) << " profile " << Detector::profileToString(b->algorithm_.profile()) << std::endl;
      total_size += b->total_size_;
    }
  }
  // Sum up blocks size
  std::cout << "Files " << files_.size() << " uncompressed size " << formatNumber(total_size) << std::endl;
}