{-# LANGUAGE FlexibleInstances #-}

import Control.Applicative
import Control.DeepSeq
import Data.List (sort,unfoldr)
import Data.Word
import System.Process
import System.Vacuum.Cairo (viewFile)
import Test.QuickCheck

view x = rnf x `seq` do
  viewFile "temp.svg" x
  system "opera temp.svg"

data Heap a = E | T a (Heap a) (Heap a) deriving Show
-- We actually require that the root has an empty right child. It irks me.
--data Heap a = EmptyHeap | Heap a (HeapTree a)

instance NFData a => NFData (Heap a) where
  rnf E = ()
  rnf (T x d r) = rnf (x,d,r)

size E = 0
size (T x a b) = 1 + size a + size b

isEmpty E = True
isEmpty _ = False

isProperHeap :: Word -> Heap Word -> Bool
isProperHeap x (T y d r) = x <= y && isProperHeap y d && isProperHeap x r
isProperHeap x E = True

isProperRoot E = True
isProperRoot (T x hs E) = isProperHeap x hs
isProperRoot _ = False


findMin (T x _ _) = x

insert x h = merge (T x E E) h
insertList xs h = foldl (flip insert) h xs

merge h E = h
merge E h = h
merge h1@(T x hs1 E) h2@(T y hs2 E)
  | x <= y    = T x (T y hs2 hs1) E
  | otherwise = T y (T x hs1 hs2) E

mergePairs E = E
mergePairs h@(T x _ E) = h
mergePairs (T x hs1 (T y hs2 hs)) =
  merge (merge (T x hs1 E) (T y hs2 E)) (mergePairs hs)

deleteMin (T _ hs E) = mergePairs hs

delete x = go
  where
    go E = E
    go (T y d r)
        | x == y = merge d r
        | otherwise = T y (go d) (go r)

heapsort xs = unfoldr f (insertList xs E)
  where
    f E = Nothing
    f h = Just (findMin h, deleteMin h)




deepCheck p = quickCheckWith (stdArgs { maxSuccess = 10000}) p
instance (Ord a, Arbitrary a) => Arbitrary (Heap a) where
  arbitrary = frequency [(1, return E), (10, insert <$> arbitrary <*> arbitrary)]
  {-shrink E = [E]
  shrink x@(T _ E E) = [E]
  shrink (T x h1 h2) = let xs = (T x <$> shrink h1 <*> shrink h2) in xs ++ concatMap shrink xs-}

newtype NonEmptyHeap a = NonEmptyHeap a
instance (Ord a, Arbitrary a) => Arbitrary (NonEmptyHeap (Heap a)) where
  arbitrary = NonEmptyHeap <$> (insert <$> arbitrary <*> arbitrary)
instance Show a => Show (NonEmptyHeap a) where
  show (NonEmptyHeap x) = show x
  showsPrec n (NonEmptyHeap x) = showsPrec n x

prop_merge_keeps_proper = (\x y -> isProperRoot x && isProperRoot y ==> isProperRoot (merge x y))
prop_merge_size = (\x y -> isProperRoot x && isProperRoot y ==> size (merge x y) == size x + size y)
prop_insert_keeps_proper = (\x y -> isProperRoot x ==> isProperRoot (insert y x))
prop_insert_size = (\x y -> isProperRoot x ==> size (insert y x) == size x + 1)
prop_insert_min (NonEmptyHeap x) y =
    isProperRoot x ==> min oldMin newMin == min oldMin y
    where
      newMin = findMin (insert y x)
      oldMin = findMin x
prop_insert_list_findMin (NonEmpty ys) = isProperRoot x && findMin x == minimum ys
  where
    x = insertList ys E

prop_deleteMin_keeps_proper = (\(NonEmptyHeap x) -> isProperRoot x ==> isProperRoot (deleteMin x))
prop_deleteMin_size = \(NonEmptyHeap x) -> isProperRoot x ==> size (deleteMin x) == size x - 1
prop_deleteMin_insert_min (NonEmptyHeap x) = isProperRoot x ==>
  findMin x'' == findMin x && isProperRoot x' && isProperRoot x''
  where
    x' = insert (findMin x) x
    x'' = deleteMin x'
prop_deleteMin_list_second (NonEmpty ys) = not (null (tail ys)) ==>
  findMin x' == head ys' && findMin x'' == head (tail ys') && isProperRoot x' && isProperRoot x''
  where
    x' = insertList ys E
    x'' = deleteMin x'
    ys' = sort ys

prop_insert1_delete_proper x y = isProperRoot x ==> isProperRoot (delete y (insert y x))
prop_insert_delete_proper x (NonEmpty ys) = isProperRoot x ==> isProperRoot (deleteAll ys (insertAll ys x))
deleteAll [] x = x
deleteAll (y:ys) x = deleteAll ys (delete y x)
insertAll [] x = x
insertAll (y:ys) x = insertAll ys (insert y x)

prop_heapsort xs = heapsort (xs :: [Int]) == sort xs

main = do
    deepCheck prop_insert1_delete_proper
    deepCheck prop_insert_delete_proper
    deepCheck prop_merge_keeps_proper
    deepCheck prop_merge_size
    deepCheck prop_insert_keeps_proper
    deepCheck prop_insert_size
    deepCheck prop_insert_min
    deepCheck prop_insert_list_findMin
    deepCheck prop_deleteMin_keeps_proper
    deepCheck prop_deleteMin_size
    deepCheck prop_deleteMin_insert_min
    deepCheck prop_deleteMin_list_second
    deepCheck prop_heapsort
