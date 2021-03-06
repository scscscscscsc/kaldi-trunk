#!/bin/bash
. ./cmd.sh ## You'll want to change cmd.sh to something that will work on your system.
           ## This relates to the queue.

expindex=test_mfcc      # experiment index

#Now begin train DNN systems on multi data
. ./path.sh
dir=exp/${expindex}
ali=exp/tri2b_multi_ali_si84
ali_dev=exp/tri2b_multi_ali_dev_0330
feats=data-fbank
echo "Layerwise pretrain DNN training"
  $cuda_cmd $dir/_train_nnet.log \
    steps/nnet/train_new.sh --hid-layers 2 --learn-rate 0.008 --network-type "dnn" \
    $feats/train_si84_multi $feats/dev_0330 data/lang $ali $ali_dev $dir || exit 1;

#Make graph and decode
echo "make graph and decode for average"
utils/mkgraph.sh data/lang_test_tgpr_5k $dir $dir/graph_tgpr_5k || exit 1;
steps/nnet/decode.sh --nj 8 --acwt 0.10 --config conf/decode_dnn.config \
  ${dir}/graph_tgpr_5k $feats/test_eval92 $dir/decode_tgpr_5k_eval92 || exit 1;

##make graph and decode for ABCD
#echo "make graph and decode for ABCD" 
#for x in test_A test_B test_C test_D;do
#steps/nnet/decode.sh --nj 8 --acwt 0.10 --config conf/decode_dnn.config \
#  ${dir}/graph_tgpr_5k $feats/$x $dir/decode_tgpr_5k_$x || exit 1;
#done

