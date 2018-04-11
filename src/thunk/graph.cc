/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include "graph.hh"

#include <stdexcept>

#include "ggutils.hh"
#include "thunk.hh"
#include "thunk_reader.hh"
#include "thunk_writer.hh"

using namespace std;
using namespace gg::thunk;

string ExecutionGraph::add_thunk( const string & orig_hash )
{
  size_t found = orig_hash.find( "#" );
  const string hash = ( found != string::npos )
      ? orig_hash.substr( 0, found )
      : orig_hash;

  const string & updated = updated_hash( hash );

  if ( thunks_.count( updated ) ) {
    return updated;
  }

  if ( thunks_.count( hash ) ) {
    return hash;
  }

  Thunk thunk { move( ThunkReader::read( gg::paths::blob_path( hash ), hash ) ) };

  /* creating the entry */
  referencing_thunks_[ hash ];

  for ( const Thunk::DataItem & item : thunk.values() ) {
    value_dependencies_.emplace( item.first );
  }

  for ( const Thunk::DataItem & item : thunk.executables() ) {
    executable_dependencies_.emplace( item.first );
  }

  vector<pair<string, string>> updates_to_thunk;

  for ( const Thunk::DataItem & item : thunk.thunks() ) {
    size_t found_ref = ( item.first ).find( "#" );
    const string mod_hash = ( found_ref != string::npos )
        ? ( item.first ).substr( 0, found_ref )
        : item.first;

    const string item_updated = add_thunk( mod_hash );
    referencing_thunks_[ item_updated ].emplace( hash );

    if ( item_updated != mod_hash ) {
      updates_to_thunk.emplace_back( mod_hash, item_updated );
    }
  }

  for ( const pair<string, string> & update : updates_to_thunk ) {
    thunk.update_data( update.first, update.second );
  }

  thunks_.emplace( piecewise_construct,
                   forward_as_tuple( hash ),
                   forward_as_tuple( move( thunk ) ) );

  return hash;
}

void ExecutionGraph::update_hash( const string & old_hash, const string & new_hash )
{
  /* updating the hash chain */
  if ( gg::hash::type( new_hash ) == gg::ObjectType::Thunk ) {
    if ( original_hashes_.count( old_hash ) == 0 ) {
      original_hashes_[ new_hash ] = old_hash;
      updated_hashes_[ old_hash ] = new_hash;
    }
    else {
      original_hashes_[ new_hash ] = original_hashes_[ old_hash ];
      updated_hashes_[ original_hashes_[ old_hash ] ] = new_hash;
      original_hashes_.erase( old_hash );
    }
  }

  /* updating the thunks that are referencing this thunk */
  for ( const string & referencing_thunk_hash : referencing_thunks_.at( old_hash ) ) {
    Thunk & referencing_thunk = thunks_.at( referencing_thunk_hash );
    referencing_thunk.update_data( old_hash, new_hash );
  }

  /* we don't need the old thunk entry */
  thunks_.erase( old_hash );

  /* we don't need the old referencing thunks list */
  referencing_thunks_[ new_hash ] = move( referencing_thunks_.at( old_hash ) );
  referencing_thunks_.erase( old_hash );
}

Optional<unordered_set<string>>
ExecutionGraph::force_thunk( const string & old_hash, const string & new_hash )
{
  if ( thunks_.count( old_hash ) == 0 ) {
    return { false };
  }

  string actual_new_hash = new_hash;
  unordered_set<string> next_to_execute;
  const gg::ObjectType new_type = gg::hash::type( new_hash );

  /* the old thunk has returned a new thunk. this is not a pipe dream. */
  if ( new_type == gg::ObjectType::Thunk ) {
    actual_new_hash = add_thunk( new_hash );
  }

  update_hash( old_hash, actual_new_hash );

  for ( const string & referencing_thunk_hash : referencing_thunks_.at( actual_new_hash ) ) {
    Thunk & referencing_thunk = thunks_.at( referencing_thunk_hash );

    if ( referencing_thunk.can_be_executed() ) {
      string referencing_thunk_new_hash = ThunkWriter::write( referencing_thunk );
      thunks_.emplace( piecewise_construct,
                       forward_as_tuple( referencing_thunk_new_hash ),
                       forward_as_tuple( move( referencing_thunk ) ) );
      update_hash( referencing_thunk_hash, referencing_thunk_new_hash );
      next_to_execute.emplace( move( referencing_thunk_new_hash ) );
    }
    else {
      auto ref_thunk_thunks = referencing_thunk.thunks();
      bool do_update = false;
      for ( const Thunk::DataItem & dep_item : ref_thunk_thunks ) {
        size_t found = ( dep_item.first ).find( "#" );
        if ( found != string::npos ) {
          auto result = gg::cache::check( dep_item.first );
          if ( result.initialized() ) {
            referencing_thunk.update_data( dep_item.first, result->hash );
            do_update = true;
          }
        }
      }
      if ( do_update ) {
        string referencing_thunk_new_hash = ThunkWriter::write( referencing_thunk );
        referencing_thunk.set_hash( referencing_thunk_new_hash );

        thunks_.emplace( piecewise_construct,
                         forward_as_tuple( referencing_thunk_new_hash ),
                         forward_as_tuple( move( referencing_thunk ) ) );
        update_hash( referencing_thunk_hash, referencing_thunk_new_hash );
        next_to_execute.emplace( move( referencing_thunk_new_hash ) );
      }
    }
  }

  if ( new_type == gg::ObjectType::Thunk ) {
    next_to_execute = order_one_dependencies( actual_new_hash );
  }
  else {
    /* the thunk has been reducted to a value. we don't need to keep
    the list of thunks that are referencing it anymore. */
    referencing_thunks_.erase( actual_new_hash );
  }

  return { true, move( next_to_execute ) };
}

unordered_set<string> ExecutionGraph::order_one_dependencies( const string & orig_hash ) const
{
  size_t found = orig_hash.find( "#" );
  const string hash = ( found != string::npos )
      ? orig_hash.substr( 0, found )
      : orig_hash;

  if ( thunks_.count( hash ) == 0 ) {
    throw runtime_error( "thunk hash not found in the execution graph" );
  }

  const Thunk & thunk = thunks_.at( hash );

  if ( thunk.can_be_executed() ) {
    return { hash };
  }

  unordered_set<string> result;

  for ( const Thunk::DataItem & item : thunk.thunks() ) {
    auto subresult = order_one_dependencies( item.first );
    result.insert( subresult.begin(), subresult.end() );
  }

  return result;
}

string ExecutionGraph::updated_hash( const string & original_hash ) const
{
  return updated_hashes_.count( original_hash ) ? updated_hashes_.at( original_hash )
                                                : original_hash;
}

string ExecutionGraph::original_hash( const string & updated_hash ) const
{
  return original_hashes_.count( updated_hash ) ? original_hashes_.at( updated_hash )
                                                : updated_hash;
}

const vector<Thunk> ExecutionGraph::get_thunks( const string & hash,
                                                const unordered_set<string> & /*rem_targs*/ ) const
{
  deque<Thunk> thunk_candidates;
  thunk_candidates.push_back( thunks_.at( hash ) );

  vector<Thunk> group;
  group.push_back( thunks_.at( hash ) );

  while ( !thunk_candidates.empty() ) {
    Thunk next_thunk = thunk_candidates.front();
    thunk_candidates.pop_front();

    if ( referencing_thunks_.find( next_thunk.hash() ) != referencing_thunks_.end() ) {
      auto ref_thunks = referencing_thunks_.at( next_thunk.hash() );
      if ( ref_thunks.size() == 1 ) {
        string lone_ref = *( ref_thunks.begin() );
        //if ( rem_targs.find( lone_ref ) == rem_targs.end() ) {
          if ( thunks_.at( lone_ref ).thunks().size() == 1 ) {
            /*for ( const string & rh : rem_targs ) {
              if ( thunks_.find( rh ) != thunks_.end() ) {
                auto all_thunks = thunks_.at( rh ).thunks();
                if ( all_thunks.find( lone_ref ) == all_thunks.end() ) {*/
                  thunk_candidates.push_back( thunks_.at( lone_ref ) );
                  group.push_back( thunks_.at( lone_ref ) );
                //}
              //}
            //}
          }
        //}
      }
    }
  }

  // cout << "Group size: " << group.size() << endl;
  return group;
}

