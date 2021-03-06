#include <stdlib.h>
#include <stdio.h>
#include <map>
#include <list>
#include <iostream>
#include <string>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "utils.h"
#include "process.h"
#include "process.C"
#include "sight_structure.h"
using namespace std;
using namespace sight;
using namespace sight::structure;

//#define VERBOSE

// A unique signature that differentiates tags of incompatible types from each other
class tagGroup {
  public:
  properties::tagType type;
  std::string objName;
  //Callpath cp;
  // A list of key strings provided by the object-specific merger
  std::list<std::string> key;
  
  tagGroup(properties::tagType type, const properties* props, std::list<std::string> key) {
    this->type = type;
    objName = props->name();
/*    if(properties::exists(props->begin(), "callPath")) {
    	//cout << objName<*<": "<<properties::get(props->begin(), "callPath")<<endl;
      cp = make_path(properties::get(props->begin(), "callPath"));
    }*/
    this->key = key;
  }
  
  bool operator==(const tagGroup& that) const
  { return type==that.type && objName==that.objName /*&& cp==that.cp */&& ((key.size()==0 && that.key.size()==0) || key==that.key); }
  
  bool operator<(const tagGroup& that) const
  { return type< that.type ||
          (type==that.type && objName< that.objName) ||
          (type==that.type && objName==that.objName  && key<that.key); }
}; // class tagGroup

// The different types of merging 
typedef enum {commonMerge, // merge the common parts of the logs
              zipper, // gather data from all the logs without merging any common parts
              diff    // merge the common log parts, while highlighting their differences
             } mergeType;

// Returns the string representation of the given mergeType
string mergeType2Str(mergeType mt) {
  switch(mt) {
    case commonMerge: return "common";
    case zipper: return "zipper";
    case diff:   return "diff";
  }
  assert(0);
  return "";
}

// Given a string repreesntation of the given mergeType, returns the corresponding enum
mergeType str2MergeType(string mtStr) {
  if(mtStr == "common") return commonMerge;
  if(mtStr == "zipper") return zipper;
  if(mtStr == "diff")   return diff;
  cerr << "ERROR: Unknown merge type \""<<mtStr<<"\"!"<<endl;
  assert(0);
}
// parsers - Vector of parsers from which information will be read
// nextTag - If merge() is called recursively after a given tag is entered on some but not all the parsers,
//    contains the information of this entered tag.
// readyForTag - Records whether we're ready to read another tag from each parser
// activeParser - Records whether each parser is still active or whether we've reached its end and the 
//    number of active parsers
// tag2stream - Maps the next observed tag name/type to the input streams on which tags that match 
//    this signature were read
// numTextTags - Records the number of parsers on which the last read tag was text. We alternate between reading text 
//    and reading tags and if text is read from some but not all parsers, the contributions from the other 
//    parsers are considered to be the empty string.
// variantStackDepth - depth of calls to merge(). Each level of recursion corresponds to some structural
//    difference between the different output streams where in one parser we enter a tag and in another
//    we do not. For each such difference we call merge() recursively on all the parsers that enter a tag
//    and exit this call when we exit this tag.
// out - stream to which data will be written
//
// Returns the number of tags emitted during the course of this merge.
int merge(vector<FILEStructureParser*>& parsers, 
                   vector<pair<properties::tagType, const properties*> >& nextTag, 
                   std::map<std::string, streamRecord*>& outStreamRecords,
                   std::vector<std::map<std::string, streamRecord*> >& inStreamRecords,
                   std::vector<bool>  readyForTag,
                   std::vector<bool>& activeParser,
                   map<tagGroup, list<int> > tag2stream,
                   int numTextTags,
                   int variantStackDepth,
                   structure::dbgStream& out, 
                   mergeType mt,
                   string indent);

// Given a vector of entities and a list of indexes within the vector,
// fills groupVec with just the entities at the indexes in selIdxes.
template<class EltType>
void collectGroupVectorIdx(std::vector<EltType>& vec, const std::list<int>& selIdxes, std::vector<EltType>& groupVec);

// Given a vector of entities and a vector of booleans that identify the selected indexes within the vector,
// fills groupVec with just the entities at the indexes in selIdxes.
template<class EltType>
void collectGroupVectorBool(std::vector<EltType>& vec, const std::vector<bool>& selFlags, std::vector<EltType>& groupVec);

void printStreamRecords(ostream& out, 
                        std::map<std::string, streamRecord*>& outStreamRecords,
                        std::vector<std::map<std::string, streamRecord*> >& inStreamRecords,
                        string indent);
void printTags(ostream& out,
               std::vector<bool>& activeParser,
               vector<pair<properties::tagType, const properties*> >& tags, 
               string indent);
//#define VERBOSE

int main(int argc, char** argv) {
  if(argc<3) { cerr<<"Usage: hier_merge outDir mergeType [fNames]"<<endl; exit(-1); }
  vector<FILEStructureParser*> fileParsers;
  const char* outDir = argv[1];
  mergeType mt = str2MergeType(string(argv[2]));
  for(int i=3; i<argc; i++) {
    fileParsers.push_back(new FILEStructureParser(argv[i], 10000));
  }
  #ifdef VERBOSE
  cout << "#fileParserRefs="<<fileParsers.size()<<endl;
  #endif
  
  // Set the working directory in the dbgStreamMerger class (it is a static variable). This must be done before an instance of this class is created
  // since the first and only instance of this class will read this working directory and write all output there.
  dbgStreamMerger::workDir = outDir;
    
  vector<pair<properties::tagType, const properties*> > emptyNextTag;
    
  // Initialize the streamRecords for the incoming and outgoing stream. The variant
  // ID starts as 0 as will grow deeper as we discover unmergeable differences
  // between or within the input streams.
  std::map<std::string, streamRecord*> outStreamRecords = MergeHandlerInstantiator::GetAllMergeStreamRecords(0);
  dbgStreamStreamRecord::enterBlock(outStreamRecords);
  
  std::vector<std::map<std::string, streamRecord*> > inStreamRecords;
  for(int i=0; i<fileParsers.size(); i++) {
    inStreamRecords.push_back(MergeHandlerInstantiator::GetAllMergeStreamRecords(i));
  }
  dbgStreamStreamRecord::enterBlock(inStreamRecords);
  
  // Records that we're ready to read another tag from each parser
  vector<bool> readyForTagFromAll(fileParsers.size(), true);
  // Records that each parser is still active
  vector<bool> allParsersActive(fileParsers.size(), true);
  
  // Maps the next observed tag name/type to the input streams on which tags that match this signature were read
  map<tagGroup, list<int> > tag2stream;
  
  // Records the number of parsers on which the last read tag was text. We alternate between reading text 
  // and reading tags and if text is read from some but not all parsers, the contributions from the other 
  // parsers are considered to be the empty string.
  int numTextTags = 0;
  
  merge(fileParsers, emptyNextTag, outStreamRecords, inStreamRecords, 
        readyForTagFromAll, allParsersActive,
        tag2stream, numTextTags,
        0, structure::dbg, mt, "   :");
  
  // Close all the parsers and their files
  for(vector<FILEStructureParser*>::iterator p=fileParsers.begin(); p!=fileParsers.end(); p++)
    delete *p;
  
  return 0;
}

// Given a vector of tag type/properties pairs, returns the same list but with the properties pointer
// replaced with the iterator to the start of the properties list
vector<pair<properties::tagType, properties::iterator> > beginTags(
                           vector<pair<properties::tagType, const properties*> >& tags)
{
  vector<pair<properties::tagType, properties::iterator> > ret;
  for(vector<pair<properties::tagType, const properties*> >::iterator t=tags.begin(); t!=tags.end(); t++)
    ret.push_back(make_pair(t->first, t->second->begin()));
  return ret;
}

// Merge the tags on the given incoming streams, emitting the merged tags to the outgoing stream, if this
// is requested by the merger. Returns the number of tags emitted.
int mergeTags(properties::tagType type, string objName,
              vector<pair<properties::tagType, const properties*> >& nextTag, 
              std::map<std::string, streamRecord*>& outStreamRecords,
              std::vector<std::map<std::string, streamRecord*> >& inStreamRecords,
              int& stackDepth, structure::dbgStream& out,
              string indent) {
  // Merge the properties of all tags
  //Merger* m = mergers[objName]->merge(beginTags(nextTag), outStreamRecords, inStreamRecords);
  Merger* m = (*MergeHandlerInstantiator::MergeHandlers)[objName](beginTags(nextTag), outStreamRecords, inStreamRecords, NULL);
  #ifdef VERBOSE
  cout << indent << "merged="<<m->getProps().str()<<endl;
  if(objName == "sight")
    cout << indent << "dir="<<structure::dbg.workDir<<endl;
  cout << indent << "emit="<<m->emitTag()<<", #moreTagsBefore="<<m->moreTagsBefore.size()<<", #moreTagsAfter="<<m->moreTagsAfter.size()<<endl;
  
  //printStreamRecords(cout, outStreamRecords, inStreamRecords, indent+"   ;");
  
  #endif
  
  int numTagsEmitted;
  
  // If the merger requests that this tag be emitted, do so
  if(m->emitTag()) {
    // Emit all the tags that appear before the tag that was actually read
    for(list<pair<properties::tagType, properties> >::iterator t=m->moreTagsBefore.begin(); t!=m->moreTagsBefore.end(); t++) {
    	//cout << "before: "<<(t->first == properties::enterTag? "enter": "exit")<<": "<<t->second.str()<<endl;
           if(t->first == properties::enterTag) out.enter(t->second);
      else if(t->first == properties::exitTag)  out.exit (t->second);
    }
    
    // Perform the common action of entering/exiting this tag
    if(objName == "text") {
      out << properties::get(m->getProps().find("text"), "text");
    } else {
      if(type == properties::enterTag) {
      	#ifdef VERBOSE
      	cout << "Entering props="<<m->getProps().str()<<"\n";
        #endif
        stackDepth++;
        out.enter(m->getProps());

        ((dbgStreamStreamRecord*)outStreamRecords["sight"])->push(true);
      } else {
        #ifdef VERBOSE
      	cout << "Exiting props="<<m->getProps().str()<<"\n";
      	#endif
        stackDepth--;
        // Emit the exit portion of this tag only if the entry was
        if(((dbgStreamStreamRecord*)outStreamRecords["sight"])->pop())
          out.exit(m->getProps());
      }
    }
    
    // Emit all the tags that appear after the tag that was actually read
    for(list<pair<properties::tagType, properties> >::iterator t=m->moreTagsAfter.begin(); t!=m->moreTagsAfter.end(); t++) {
           if(t->first == properties::enterTag) out.enter(t->second);
      else if(t->first == properties::exitTag)  out.exit (t->second);
    }
    
    numTagsEmitted=1;
  // If we don't need to emit the tag, still adjust the stackDepth to account for the fact
  // that the tag was read
  } else {
    if(objName != "text") {
      if(type == properties::enterTag) {
      	//cout << "Entering props="<<m->getProps().str()<<"\n";
        stackDepth++;
        ((dbgStreamStreamRecord*)outStreamRecords["sight"])->push(false);
      } else {
      	//cout << "Exiting props="<<m->getProps().str()<<"\n";
        stackDepth--;
        ((dbgStreamStreamRecord*)outStreamRecords["sight"])->pop();
      }
    }
    numTagsEmitted=0;
  }
  delete(m);
  return numTagsEmitted;
}

// Emit the tags on the given incoming streams to the outgoing stream in the order of the incoming streams
// in nextTag.
void zipperTags(properties::tagType type, string objName,
                vector<pair<properties::tagType, const properties*> >& nextTag, 
                std::map<std::string, streamRecord*>& outStreamRecords,
                std::vector<std::map<std::string, streamRecord*> >& inStreamRecords,
                int& stackDepth, structure::dbgStream& out,
                string indent) {
  
  for(vector<pair<properties::tagType, const properties*> >::iterator t=nextTag.begin(); 
      t!=nextTag.end(); t++) {
    if(t->first == properties::enterTag) {
      #ifdef VERBOSE
    	cout << "zipperTags: Entering props="<<t->second->str()<<"\n";
      #endif
      stackDepth++;
      out.enter(*t->second);
    } else if(t->first == properties::exitTag) {
      #ifdef VERBOSE
    	cout << "zipperTags: Exiting props="<<t->second->str()<<"\n";
      #endif
      stackDepth--;
      out.exit(*t->second);
    }
  }
}

// parsers - Vector of parsers from which information will be read
// nextTag - If merge() is called recursively after a given tag is entered on some but not all the parsers,
//    contains the information of this entered tag.
// readyForTag - Records whether we're ready to read another tag from each parser
// activeParser - Records whether each parser is still active or whether we've reached its end and the 
//    number of active parsers
// tag2stream - Maps the next observed tag name/type to the input streams on which tags that match 
//    this signature were read
// numTextTags - Records the number of parsers on which the last read tag was text. We alternate between reading text 
//    and reading tags and if text is read from some but not all parsers, the contributions from the other 
//    parsers are considered to be the empty string.
// variantStackDepth - depth of calls to merge(). Each level of recursion corresponds to some structural
//    difference between the different output streams where in one parser we enter a tag and in another
//    we do not. For each such difference we call merge() recursively on all the parsers that enter a tag
//    and exit this call when we exit this tag.
// out - stream to which data will be written
//
// Returns the number of tags emitted during the course of this merge.
int merge(vector<FILEStructureParser*>& parsers, 
           vector<pair<properties::tagType, const properties*> >& nextTag, 
           std::map<std::string, streamRecord*>& outStreamRecords,
           std::vector<std::map<std::string, streamRecord*> >& inStreamRecords,
           std::vector<bool>  readyForTag,
           std::vector<bool>& activeParser,
           map<tagGroup, list<int> > tag2stream,
           int numTextTags,
           int variantStackDepth,
           structure::dbgStream& out, 
           mergeType mt,
           string indent) {
  #ifdef VERBOSE
  cout << indent << "#parsers="<<parsers.size()<<", variantStackDepth="<<variantStackDepth<<", dir="<<out.workDir<<endl;
  #endif
  int numTagsEmitted = 0;
  
  #ifdef VERBOSE
  printStreamRecords(cout, outStreamRecords, inStreamRecords, indent);
  #endif
  
  // If this is not the root call to merge (variantStackDepth>1), nextTag is set to the tag entry items that were
  // just read on every parser in parsers. Otherwise, nextTag is empty
  assert((variantStackDepth==0 && nextTag.size()==0) ||
         (variantStackDepth>0  && nextTag.size()==parsers.size()));
  
  // The current depth of the nesting stack of tags that have been entered but not yet exited. If nextTag
  // is not empty, stackDepth is set to 1 to account for the entry tag read by the caller of merge(). 
  // Otherwise stackDepth is set to 0.
  //int stackDepth=(variantStackDepth==0? 0: 1);
  int stackDepth=0;
  
  int numActive = 0;
  for(vector<bool>::const_iterator p=activeParser.begin(); p!=activeParser.end(); p++)
    numActive += (*p? 1: 0);
  
  // Counts the number of times we recursively consider variant tags. The output of processing
  // such variants needs to be written to a separate uniquely-named location. These unique names
  // are generated using this counter.
  int subDirCount=0;
  
  while(numActive>0) {
    #ifdef VERBOSE
    cout << indent << "=================================, numActive="<<numActive<<"\n";
    #endif
    
    // Read the next item from each parser
    int parserIdx=0;
    for(vector<FILEStructureParser*>::iterator p=parsers.begin(); p!=parsers.end(); p++, parserIdx++) {
      #ifdef VERBOSE
      cout << indent << "readyForTag["<<parserIdx<<"]="<<readyForTag[parserIdx]<<", activeParser["<<parserIdx<<"]="<<activeParser[parserIdx]<<endl;
      #endif
      
      // If we're ready to read a tag on this parser
      if(readyForTag[parserIdx] && activeParser[parserIdx]) {
        pair<properties::tagType, const properties*> props = (*p)->next();
        //#ifdef VERBOSE
        //cout << indent << parserIdx << ": "<<(props.first==properties::enterTag? "enterTag": "exitTag")<<" "<<const_cast<properties*>(props.second)->str()<<endl;
        //#endif
        
        // If we've reached the end of this parser's data
        if(props.second->size()==0) {
          activeParser[parserIdx] = false;
          numActive--;
        } else {
          // Record the properties of the newly-read tag
          
          // If nextTag has not yet been filled in upto this index, add the info to the end of nextTag
          if(nextTag.size()<=parserIdx) {
            assert(nextTag.size()==parserIdx);
            nextTag.push_back(props/*make_pair(props.first, props.second)*/);
          // Otherwise, set this index of nextTag directly
          } else 
            nextTag[parserIdx] = props;//make_pair(props.first, props.second);
            
          // Group this parser with all the other parsers that just read a tag with the same name and type (enter/exit)
//          cout << "props.second="<<props.second->str()<<endl;
          //cout << "MergeHandlerInstantiator="<<MergeHandlerInstantiator::str()<<endl;
          //std::list<std::string> key = mergers[props.second->name()]->mergeKey(props.first, props.second->begin(), inStreamRecords[parserIdx]);
          if(MergeHandlerInstantiator::MergeKeyHandlers->find(props.second->name()) == MergeHandlerInstantiator::MergeKeyHandlers->end()) { cerr << "ERROR: no merge handler for tag \""<<props.second->name()<<"\"!"<<endl; }
          assert(MergeHandlerInstantiator::MergeKeyHandlers->find(props.second->name()) != MergeHandlerInstantiator::MergeKeyHandlers->end());
          std::list<std::string> key;
          (*MergeHandlerInstantiator::MergeKeyHandlers)[props.second->name()](props.first, props.second->begin(), inStreamRecords[parserIdx], key);
/*          cout << "key=";
          for(list<string>::iterator k=key.begin(); k!=key.end(); k++) cout << *k << " ";
          cout << endl;*/
          
          tag2stream[tagGroup(props.first, props.second, 
                              //(mt==commonMerge? 
                                /*mergers[props.second->name()]->
                                       mergeKey(props.first, props.second->begin(), inStreamRecords[parserIdx])*/
                                key/*: 
                                list<string>())*/)
                    ].push_back(parserIdx);
                    
          // Record whether we read a text tag on any parser
          numTextTags += (props.second->name() == "text"? 1: 0);
          
          // We've just read a tag and are thus not ready for another on this incoming parser
          // until this one is processed
          readyForTag[parserIdx] = false;
        }
      }
    }
    
    #ifdef VERBOSE
    cout << indent << "numTextTags="<<numTextTags<<", numActive="<<numActive<<", nextTag("<<nextTag.size()<<")"<<endl;
    #endif
    if(numActive==0) break;
    #ifdef VERBOSE
    printTags(cout, activeParser, nextTag, indent+"   .");
    #endif
    
    // If we read text on any parser, emit the text immediately
    if(numTextTags>0) {
      for(map<tagGroup, list<int> >::iterator i=tag2stream.begin(); i!=tag2stream.end(); i++) {
      //assert(tag2stream.find(make_pair(properties::enterTag, "text")) != tag2stream.end());
      if(i->first.type==properties::enterTag && i->first.objName=="text") {
        // Parsers on which we read text
        const list<int>& textParsers = i->second; //tag2stream[make_pair(properties::enterTag, "text")];
        //cout << indent << "#textParsers="<<textParsers.size()<<endl;
        assert(textParsers.size()>0);
        for(list<int>::const_iterator j=textParsers.begin(); j!=textParsers.end(); j++) {
          //cout << indent << "j="<<*j<<", #nextTag="<<nextTag.size()<<endl;
          assert(activeParser[*j]);
        }
        
        // Contains the next read tag of just this group
        vector<pair<properties::tagType, const properties*> > groupNextTag;
        collectGroupVectorIdx<pair<properties::tagType, const properties*> >(nextTag, textParsers, groupNextTag);
        
        // Gather the streamRecords of just the incoming streams within this group
        std::vector<std::map<std::string, streamRecord*> > groupInStreamRecords;
        collectGroupVectorIdx<std::map<std::string, streamRecord*> >(inStreamRecords, textParsers, groupInStreamRecords);
        
        ///if(mt == commonMerge)
          numTagsEmitted +=
            mergeTags(properties::enterTag, "text", 
                      groupNextTag, outStreamRecords, groupInStreamRecords,
                      stackDepth, out, indent+"   .");
        /*else if(mt == zipper) {
          zipperTags(properties::enterTag, "text", 
                    groupNextTag, outStreamRecords, groupInStreamRecords,
                    stackDepth, out, indent+"   .");
          numTagsEmitted += groupNextTag.size();
        }*/
        for(list<int>::const_iterator j=textParsers.begin(); j!=textParsers.end(); j++)
          // We're ready to read a new tag on this parser
          readyForTag[*j] = true;      
        
        // Reset the tag2stream key associated with text reading
        tag2stream.erase(i);
          
        // Reset numTextTags
        numTextTags = 0;
        
        break;
      } }
      // We'll now repeat the loop and read more tags on all the parsers from which we just read text
      // If we only entered or exited tags
    
    // If we did not read text on any parser
    } else {
      #ifdef VERBOSE
      cout << indent << tag2stream.size() << " key groups"<<endl;
      #endif
      // Group all the streams with the same next tag name/type. Since all parsers must have the same
      // stack of tags that have been entered, if we read the exit of a tag on any parser(s), they all 
      // must exit the same tag. However, different parsers may enter different tags.
      //
      // If there is just one group, then we merge the properties of the tags (all same type)
      // read from all the parsers, and perform the enter/exit action of this group
      if(tag2stream.size()==1) {
        if(MergeHandlerInstantiator::MergeKeyHandlers->find(tag2stream.begin()->first.objName) == MergeHandlerInstantiator::MergeKeyHandlers->end()) 
          cerr << "ERROR: cannot find a merger for tag \""<<tag2stream.begin()->first.objName<<"\"!"<<endl;
        assert(MergeHandlerInstantiator::MergeKeyHandlers->find(tag2stream.begin()->first.objName) != MergeHandlerInstantiator::MergeKeyHandlers->end());
        
        // Contains the next read tag of just the active incoming streams
        vector<pair<properties::tagType, const properties*> > groupNextTag;
        collectGroupVectorBool<pair<properties::tagType, const properties*> >(nextTag, activeParser, groupNextTag);
        
        // Gather the streamRecords of just the incoming streams within this group
        std::vector<std::map<std::string, streamRecord*> > groupInStreamRecords;
        collectGroupVectorBool<std::map<std::string, streamRecord*> >(inStreamRecords, activeParser, groupInStreamRecords);

        //cout << indent << "calling mergeTags, objName="<<tag2stream.begin()->first.objName<<endl;
        // Merge the tags if we're merging common log components or if we're zippering 
        // but this is the highest-level sight object that does need to be merged
        //if(mt == commonMerge || (mt == zipper && tag2stream.begin()->first.objName=="sight"))
          numTagsEmitted +=
            mergeTags(tag2stream.begin()->first.type, tag2stream.begin()->first.objName,
                    groupNextTag, outStreamRecords, groupInStreamRecords,
                    stackDepth, out, indent+"   .");
        /*else if(mt == zipper) {
          zipperTags(tag2stream.begin()->first.type, tag2stream.begin()->first.objName,
                  groupNextTag, outStreamRecords, groupInStreamRecords,
                  stackDepth, out, indent+"   .");
          numTagsEmitted += groupNextTag.size();
        }*/
        //cout << indent << "post-mergeTags() stackDepth="<<stackDepth<<endl;
        
        // Record that we're ready for more tags on all the parsers
        for(vector<bool>::iterator i=readyForTag.begin(); i!=readyForTag.end(); i++)
          *i = true;
        
        /*// Reset readyForTag and tag2stream to forget this tag on the parsers within the current group
        // and get ready to read more from them.
        for(list<int>::const_iterator i=tag2stream.begin()->second.begin(); i!=tag2stream.begin()->second.end(); i++)
          readyForTag[*i] = true;*/
        
        // Reset tag2stream since we'll be reloading it based on the next tag read from each parser
        tag2stream.clear();
        
        // If we've exited out of the highest-level tag at this variant level, exit out to the parent
        // call to merge() unless this is the root call to merge()
        if(stackDepth==0 && variantStackDepth>0)
          return numTagsEmitted;
      }
      
      // If there are multiple groups consider the ones that entered a tag. These clearly diverge from
      // each other and from groups that exited a tag since all the parsers must have entered this
      // tag and now some are trying to exit while others are trying to enter another, more deeply-nested tag.
      // Thus, we recursively call merge() on each group that is trying to enter a tag, allowing it to process
      // this tag until it is exited. The groups that are trying to exit a tag are left alone to wait for the
      // enterers to complete. Eventually all the parsers that entered a tag will exit it and thus exit the 
      // merge() call. At this point we'll read the next tag from them and see if they can be merged.
      else {
        // Each group gets a separate copy of outStreamRecords. This list stores them all so that we can later merge them.
        vector<std::map<std::string, streamRecord*> > allGroupOutStreamRecords;
        
        #ifdef VERBOSE
        cout << indent << "::: "<<tag2stream.size()<<" Variants ::::"<<endl;
        #endif
        
        dbgStreamStreamRecord::enterBlock(outStreamRecords);
        
        // Iterate over all the groups that entered a tag
        int variantID=0;
        // Sub-directories that hold the contents of all the variants
        vector<string> variantSubDirs;
        
        // Check if we're at the tag exits on all streams
        
        for(map<tagGroup, list<int> >::iterator ts=tag2stream.begin(); ts!=tag2stream.end(); variantID++) {
          #ifdef VERBOSE
          cout << indent << "    Variant "<<variantID<<", "<<(ts->first.type == properties::enterTag? "enter": "exit")<<", "<<ts->first.objName<<endl;
          #endif
          
          if(ts->first.type == properties::enterTag) {
            assert(ts->second.size()>0);
            
            // Contains the parsers of just this group
            vector<FILEStructureParser*> groupParsers;
            collectGroupVectorIdx<FILEStructureParser*>(parsers, ts->second, groupParsers);
            
            // Contains the next read tag of just this group
            vector<pair<properties::tagType, const properties*> > groupNextTag;
            collectGroupVectorIdx<pair<properties::tagType, const properties*> >(nextTag, ts->second, groupNextTag);
              
            // Create a copy of outStreamRecords for this group
            std::map<std::string, streamRecord*> groupOutStreamRecords;
            for(std::map<std::string, streamRecord*>::iterator o=outStreamRecords.begin(); o!=outStreamRecords.end(); o++)
              groupOutStreamRecords[o->first] = o->second->copy(variantID);
            allGroupOutStreamRecords.push_back(groupOutStreamRecords);
            
            // Gather the streamRecords of just the incoming streams within this group
            std::vector<std::map<std::string, streamRecord*> > groupInStreamRecords;
            collectGroupVectorIdx<std::map<std::string, streamRecord*> >(inStreamRecords, ts->second, groupInStreamRecords);
            
            // Gather the activeParser of just the incoming streams within this group
            std::vector<bool> groupActiveParser;
            collectGroupVectorIdx<bool>(activeParser, ts->second, groupActiveParser);
            
            // We are not ready for another tag on any stream within this group. We will process the tags
            // we just read
            std::vector<bool> groupNotReadyForTag(ts->second.size(), false);
              
            // The tag2stream map of this group, which maps all the group's streams to the
            // common tag
            map<tagGroup, list<int> > groupTag2stream;
            groupTag2stream.insert(*ts);
              
            // Note: This code assumes that the disagreement point is not a text tag.
            //       If we wish to remove this constraint, we'll need to adjust the code
            //       that reads the next tag to not read the next tag in this case but still
            //       check if the tags that are currently in groupNextTag are text.
            
            // <<< Recursively Call merge()
            if(mt == zipper) {
              #ifdef VERBOSE
              cout << indent << "<<<<<<<<<<<<<<<<<<<<<<"<<endl;
              #endif
              //cout << indent << "start outLocation="<<((dbgStreamStreamRecord*)groupOutStreamRecords["sight"])->getLocation().str()<<endl;
              int numVariantTagsEmitted = 
                merge(groupParsers, groupNextTag, 
                      groupOutStreamRecords, groupInStreamRecords, 
                      groupNotReadyForTag, groupActiveParser,
                      groupTag2stream,
                      numTextTags,
                      variantStackDepth+1, out, 
                      mt, 
                      indent+"   :");
              //cout << indent << "end outLocation="<<((dbgStreamStreamRecord*)groupOutStreamRecords["sight"])->getLocation().str()<<endl;
              //cout << indent << "end numVariantTagsEmitted="<<numVariantTagsEmitted<<endl;
              #ifdef VERBOSE
              cout << indent << ">>>>>>>>>>>>>>>>>>>>>>"<<endl;
              #endif
            } else {
              string subDir = txt()<<out.workDir<<"/"<<"var_"<<subDirCount;
              //cout << "subDir="<<subDir<<endl;
              
              // Create the directory structure for the structural information
              // Main output directory
              createDir(subDir, "");
            
              // Directory where client-generated images will go
              string imgDir = createDir(subDir, "html/dbg_imgs");
              
              // Directory that widgets can use as temporary scratch space
              string tmpDir = createDir(subDir, "html/tmp");
              
              //cout << indent << "Creating groupStream\n";
              structure::dbgStream groupStream(NULL, txt()<<"Variant "<<subDirCount, subDir, imgDir, tmpDir);
              //cout << indent << "Created groupStream\n";
              
              #ifdef VERBOSE
              cout << indent << "<<<<<<<<<<<<<<<<<<<<<<"<<endl;
              #endif
              //cout << indent << "start outLocation="<<((dbgStreamStreamRecord*)groupOutStreamRecords["sight"])->getLocation().str()<<endl;
              int numVariantTagsEmitted = 
                merge(groupParsers, groupNextTag, 
                      groupOutStreamRecords, groupInStreamRecords, 
                      groupNotReadyForTag, groupActiveParser,
                      groupTag2stream,
                      numTextTags,
                      variantStackDepth+1, groupStream, 
                      mt, 
                      indent+"   :");
              //cout << indent << "end outLocation="<<((dbgStreamStreamRecord*)groupOutStreamRecords["sight"])->getLocation().str()<<endl;
              //cout << indent << "end numVariantTagsEmitted="<<numVariantTagsEmitted<<endl;
              #ifdef VERBOSE
              cout << indent << ">>>>>>>>>>>>>>>>>>>>>>"<<endl;
              #endif
              
              // If we emitted at least one tag within this variant, we record this variant 
              // to include it in the [variants] tag that points to it.
              if(numVariantTagsEmitted>0) {
                variantSubDirs.push_back(subDir);
  
                subDirCount++;
              // Otherwise, if this variant is empty, we delete it
              } else {
                rmdir(subDir.c_str());
              }
             
            }
            // >>>
            
            // We're done with processing the tag that was just entered
            
            // Update activeParsers[] based on the reading activity that occured inside the merge() call.
            // If entry/exit tags were balanced we'd be guaranteed that all of the grou's parsers would
            // be active but since the log generating application may terminate prematurely, we may hit
            // the end of its log inside this merge call.
            // groupIdx - the index of a given parser within this group (groupParsers[])
            // globalIdx - the index of the same parser within the entire parsers[] vector
            int groupIdx=0;
            for(list<int>::iterator globalIdx=ts->second.begin(); globalIdx!=ts->second.end(); globalIdx++, groupIdx++) {
              assert(activeParser[*globalIdx]);
              
              // If the current parser was previously active and has now terminated, update state
              if(groupActiveParser[groupIdx]==false) {
                activeParser[*globalIdx]=false;
                numActive--;
              }
            }
                        
            // Reset readyForTag and tag2stream to forget this tag on the parsers within the current group
            // and get ready to read more from them.
            for(list<int>::const_iterator i=ts->second.begin(); i!=ts->second.end(); i++)
              readyForTag[*i] = true;
            tag2stream.erase(ts++);
            
            break;
          // Do nothing for exit tags since these parsers wait for the ones that entered tags to complete their
          // processing of these tags
          } else if(ts->first.type == properties::exitTag) {
            ts++;
          }
        } // Iterate over all the groups that entered a tag
        
        // Resume the streamRecord of the outgoing stream from the sub-streams of the group's variants
        for(map<string, streamRecord*>::iterator o=outStreamRecords.begin(); o!=outStreamRecords.end(); o++) {
          o->second->resumeFrom(allGroupOutStreamRecords);
          // Delete the sub-streams streamRecords for each variant in this group
          for(vector<std::map<std::string, streamRecord*> >::iterator i=allGroupOutStreamRecords.begin(); 
              i!=allGroupOutStreamRecords.end(); i++)
          delete (*i)[o->first];
        }

        // If we produced any output tags within the above variants
        if(variantSubDirs.size()>0) {        
          // Output the [variant] tag that points to the directories that hold the contents of each variant
          //out.ownerAccessing();
          properties variantProps;
          map<string, string> pMap;
          for(int v=0; v<variantSubDirs.size(); v++)
            pMap[txt()<<"var_"<<v] = variantSubDirs[v];
          variantProps.add("variants", pMap);
          out.tag(variantProps);
        }
        dbgStreamStreamRecord::exitBlock(outStreamRecords);
      }
    } // If we only entered or exited tags
  } // while(numActive>0)
  
  // We reach this point if all parsers have terminated
  
  return numTagsEmitted;
}

// Given a vector of entities and a list of indexes within the vector,
// fills groupVec with just the entities at the indexes in selIdxes.
template<class EltType>
void collectGroupVectorIdx(std::vector<EltType>& vec, const std::list<int>& selIdxes, std::vector<EltType>& groupVec) {
  for(list<int>::const_iterator i=selIdxes.begin(); i!=selIdxes.end(); i++) {
    assert(*i < vec.size());
    groupVec.push_back(vec[*i]);
  }
}

// Given a vector of entities and a vector of booleans that identify the selected indexes within the vector,
// fills groupVec with just the entities at the indexes in selIdxes.
template<class EltType>
void collectGroupVectorBool(std::vector<EltType>& vec, const std::vector<bool>& selFlags, std::vector<EltType>& groupVec) {
  int idx=0;
  for(vector<bool>::const_iterator i=selFlags.begin(); i!=selFlags.end(); i++, idx++) {
    if(*i) groupVec.push_back(vec[idx]);
  }
}


void printStreamRecords(ostream& out, 
                        std::map<std::string, streamRecord*>& outStreamRecords,
                        std::vector<std::map<std::string, streamRecord*> >& inStreamRecords,
                        string indent) {
  out << indent << "outStreamRecords="<<endl;
  for(std::map<std::string, streamRecord*>::iterator o=outStreamRecords.begin(); o!=outStreamRecords.end(); o++)
    out << indent << "    "<<o->first<<": "<<o->second->str(indent+"    ")<<endl;
  
  out << indent << "inStreamRecords="<<endl;
  int idx=0;
  for(std::vector<std::map<std::string, streamRecord*> >::iterator i=inStreamRecords.begin(); i!=inStreamRecords.end(); i++, idx++)
    for(std::map<std::string, streamRecord*>::iterator j=i->begin(); j!=i->end(); j++)
      out << indent << "    "<<idx<<": "<<j->first<<": "<<j->second->str(indent+"    ")<<endl;
}

void printTags(ostream& out,
               std::vector<bool>& activeParser,
               vector<pair<properties::tagType, const properties*> >& tags, 
               string indent) {
  //for(vector<pair<properties::tagType, properties::iterator> >::iterator t=tags.begin(); t!=tags.end(); t++)
  for(int i=0; i<tags.size(); i++) {
    if(activeParser[i])
      out << "    "<<i<<": "<<(tags[i].first==properties::enterTag? "enterTag": (tags[i].first==properties::exitTag? "exitTag": "unknownTag"))<<", "<<tags[i].second->str()<<endl;
    else
      out << "    "<<i<<": Inactive"<<endl;
  }
}
