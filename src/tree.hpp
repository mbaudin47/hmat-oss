/*
  HMat-OSS (HMatrix library, open source software)

  Copyright (C) 2014-2015 Airbus Group SAS

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

  http://github.com/jeromerobert/hmat-oss
*/

/*! \file
  \ingroup HMatrix
  \brief Templated Tree class used by ClusterTree and HMatrix.
*/
#ifndef _TREE_HPP
#define _TREE_HPP
#include <vector>
#include <list>
#include <cstddef>
#include <assert.h>

namespace hmat {

// Forward declaration
template<int N> class TreeProcedure;

/* Visitor pattern
 */
enum Visit { tree_preorder, tree_postorder, tree_inorder, tree_leaf };

/*! \brief Templated tree class.

  This class represents a tree of arity N, holding an instance of NodeData in
  its nodes.
 */
template<int N> class Tree {
public:
  /// depth of the current node in the tree
  int depth;

protected:
  /// empty for a leaf, pointeur on a vector of sons otherwise.
  std::vector<Tree<N>*> children;
public:
  /// Pointer to the father, NULL if this node is the root
  Tree* father;

public:
  Tree(Tree* _father, int _depth = 0)
    : depth(_depth), children(), father(_father) {}
  virtual ~Tree() {
    for (int i=0 ; i<children.size() ; i++)
      if (children[i])
        delete children[i];
    children.clear();
  }

  /*! \brief Insert a child in the children vector.

    If a child is already present, it is removed but not deleted.

    \param index index in the children vector
    \param child pointeur to the child
   */
  void insertChild(int index, Tree<N> *child) {
    if (children.size()<=index)
      children.resize(index+1, (Tree<N>*)NULL);
    child->father = this;
    children[index] = child;
    child->depth = depth + 1;
  }

  /*! \brief Remove a child, and delete it if necessary.
   */
  void removeChild(int index) {
    if (children[index])
    delete children[index];
    children[index] = (Tree<N>*)NULL;
  }

  /*! \brief Return the number of nodes in the tree.
   */
  int nodesCount() const {
    int result = 1;
    for (int i=0 ; i<children.size() ; i++)
      if (children[i])
        result += children[i]->nodesCount();
    return result;
  }

  /*! \brief Return the child of index, or NULL.
   */
  inline Tree *getChild(int index) const {
    assert(index<children.size());
    return children[index];
  }

  inline int nbChild() const {
    return children.size();
  }

  /*! \brief Return true if the node is a leaf (= it has no children).
   */
  inline bool isLeaf() const {
    return children.empty();
  }

  /*! \brief Return a list of nodes.

    Not used anywhere.
   */
  virtual std::list<const Tree<N>*> listNodes() const {
    std::list<const Tree<N>*> result;
    result.push_back(this);
    for (int i=0 ; i<children.size() ; i++)
      if (children[i]) {
        std::list<const Tree<N>*> childNodes = children[i]->listNodes();
        result.splice(result.end(), childNodes, childNodes.begin(), childNodes.end());
      }
    return result;
  }

 /*! \brief Return a list of leaves.
   */
  void listAllLeaves(std::vector<Tree<N>*>& leaves) const {
    if (!isLeaf()) {
      for (int i=0 ; i<children.size() ; i++)
        if (children[i])
          children[i]->listAllLeaves(leaves);
    } else {
      leaves.push_back(const_cast<Tree<N>*>(this));
    }
  }

  void walk(const TreeProcedure<N> *proc) {
    if (isLeaf()) {
      proc->visit(this, tree_leaf);
    } else {
      proc->visit(this, tree_preorder);
      bool first = true;
      for (int i=0 ; i<children.size() ; i++)
        if (children[i]) {
          if (!first)
            proc->visit(this, tree_inorder);
          first = false;
          children[i]->walk(proc);
        }
      proc->visit(this, tree_postorder);
    }
  }

};

/** Class to recursively apply a given function to all nodes of a tree
 */
template<int N> class TreeProcedure {

public:
  TreeProcedure() {}
  virtual void visit(Tree<N>* node, const Visit order) const = 0;
  virtual ~TreeProcedure() {}
};

}  // end namespace hmat

#endif  // _TREE_HPP
