import beta173.BetaTerrain173;

/**
 * P19 conservative Stage0.75 monster-potential gate.
 *
 * Model: 16 ExtraTrees regression trees, depth 5, min leaf 20, max features 0.8.
 * Trained only on the first 70% of the P18 113m-seed telemetry run to predict
 * exact P17 coarse score from pre-coarse Stage0 features. The production cutoff
 * is deliberately below the limiting known 30k+ calibration seed.
 */
public final class P19MonsterGate {
    public static final double THRESHOLD = 4.02D;
    private static final double EPSILON = 1.0E-6D;
    private static final int TREE_COUNT = 16;

    private P19MonsterGate() {}

    public static double score(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        return (
                tree0(scout, p) +
                tree1(scout, p) +
                tree2(scout, p) +
                tree3(scout, p) +
                tree4(scout, p) +
                tree5(scout, p) +
                tree6(scout, p) +
                tree7(scout, p) +
                tree8(scout, p) +
                tree9(scout, p) +
                tree10(scout, p) +
                tree11(scout, p) +
                tree12(scout, p) +
                tree13(scout, p) +
                tree14(scout, p) +
                tree15(scout, p)
        ) / TREE_COUNT;
    }

    public static boolean passes(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        // Future-proof against out-of-distribution mega-masses. These extreme
        // coherent clusters were already accepted by the trained model everywhere
        // in the 113m telemetry set, so this guard costs no measured rejection power.
        if (hasExtremeTopologySignal(p)) return true;
        return score(scout, p) >= THRESHOLD;
    }

    public static boolean hasExtremeTopologySignal(BetaTerrain173.PreparedStage0MonsterFeatures p) {
        return p.stage0Y88LargestCluster >= 14 || p.stage0Y96LargestCluster >= 14;
    }

    private static double tree0(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        if (p.stage0FullY88 <= 13.674173606873572) {
            if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.3894950105899695) {
                if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 34.52099823929416) {
                    if ((p.stage0Y96Width * p.stage0Y96Depth) <= 12.113755831547898) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.2309941259424658) {
                            return 3.064619508387458;
                        } else {
                            return 3.776290344448647;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.3400544766944227) {
                            return 3.6929329926784504;
                        } else {
                            return 5.130184508001835;
                        }
                    }
                } else {
                    if (p.stage0FullY88 <= 8.367030126679238) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.28358432579187853) {
                            return 3.8636412200004497;
                        } else {
                            return 5.678001030396703;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.19695644468550785) {
                            return 4.2972374975358445;
                        } else {
                            return 5.983985904949398;
                        }
                    }
                }
            } else {
                if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 19.372318390930218) {
                    if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 8.755598275808431) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.7529005365893975) {
                            return 2.4284280456913443;
                        } else {
                            return 5.204234122042341;
                        }
                    } else {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 12.631919959388636) {
                            return 3.985308534774936;
                        } else {
                            return 5.354870188981539;
                        }
                    }
                } else {
                    if (p.stage0Y96Width <= 3.4325716287980654) {
                        if (p.stage0FullY88 <= 9.884943111280418) {
                            return 4.808950055335404;
                        } else {
                            return 6.894008023812606;
                        }
                    } else {
                        if (p.stage0FullY112 <= 7.438540572970426) {
                            return 7.803805791889208;
                        } else {
                            return 70.62857142857143;
                        }
                    }
                }
            }
        } else {
            if (((double) p.stage0Y96LargestCluster / (p.stage0FullY96 + EPSILON)) <= 0.03870008144764666) {
                return 29.48310810810811;
            } else {
                if (scout <= 66.98755386238338) {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.6282021676234077) {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 45.70822211108553) {
                            return 6.715205184544022;
                        } else {
                            return 7.833333333333333;
                        }
                    } else {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 29.935513452596737) {
                            return 8.676333333333334;
                        } else {
                            return 16.891998682910767;
                        }
                    }
                } else {
                    if (p.stage0FullY96 <= 22.889447735571046) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.18023254216559204) {
                            return 4.857097095071906;
                        } else {
                            return 6.197163603182827;
                        }
                    } else {
                        if (p.stage0Y96LargestCluster <= 19.166081343389155) {
                            return 8.36343372557151;
                        } else {
                            return 3.3660886319845855;
                        }
                    }
                }
            }
        }
    }

    private static double tree1(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        if (p.stage0Y96Width <= 5.340061644716217) {
            if (scout <= 36.21206573919024) {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.6927915445678565) {
                    if (p.stage0FullY88 <= 6.019914244776552) {
                        if ((p.stage0Y88Width * p.stage0Y88Depth) <= 18.062172076693816) {
                            return 2.9934682115627065;
                        } else {
                            return 3.816658268480675;
                        }
                    } else {
                        if (p.stage0FullY88 <= 9.47566141070596) {
                            return 4.525432269159779;
                        } else {
                            return 6.170602118667325;
                        }
                    }
                } else {
                    if (scout <= 13.510971130575506) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.8435084208331844) {
                            return 5.43648524590164;
                        } else {
                            return 7.690210076657499;
                        }
                    } else {
                        if (((double) p.stage0FullY104 / (p.stage0FullY96 + EPSILON)) <= 0.5474072100963816) {
                            return 11.633777239709444;
                        } else {
                            return 24.872909698996654;
                        }
                    }
                }
            } else {
                if (p.stage0FullY88 <= 22.58404319807144) {
                    if (p.stage0FullY88 <= 6.070315484455934) {
                        if (p.stage0Y88Width <= 7.0787458024354395) {
                            return 2.7364038010423206;
                        } else {
                            return 3.1256729025620036;
                        }
                    } else {
                        if (p.stage0FullY88 <= 12.339951312793396) {
                            return 3.669060403263611;
                        } else {
                            return 5.6591993023192435;
                        }
                    }
                } else {
                    return 25.706484641638227;
                }
            }
        } else {
            if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.4221232727449663) {
                if ((p.stage0Y96Width * p.stage0Y96Depth) <= 112.6753390974716) {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.23443170119519663) {
                        if ((p.stage0Y96Width * p.stage0Y96Depth) <= 48.5396532968337) {
                            return 3.484301657114736;
                        } else {
                            return 4.048870811384725;
                        }
                    } else {
                        if (p.stage0Y96Depth <= 5.227148019348806) {
                            return 4.912522005136937;
                        } else {
                            return 6.132017038511226;
                        }
                    }
                } else {
                    if ((p.stage0FullY96 + p.stage0FullY104 + p.stage0FullY112) <= 24.901042636903487) {
                        if (p.stage0FullY96 <= 8.324022573569799) {
                            return 4.4065413200063;
                        } else {
                            return 5.765316102188378;
                        }
                    } else {
                        if (p.stage0FullY88 <= 17.237596362939847) {
                            return 6.092022357723577;
                        } else {
                            return 7.261791161517404;
                        }
                    }
                }
            } else {
                if (((double) p.stage0FullY104 / (p.stage0FullY96 + EPSILON)) <= 0.8119156862664575) {
                    if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.22998616920017553) {
                        if ((p.stage0FullY96 + p.stage0FullY104 + p.stage0FullY112) <= 44.8194846673028) {
                            return 10.659845559845559;
                        } else {
                            return 64.96666666666667;
                        }
                    } else {
                        if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.6782504698840748) {
                            return 8.608031028975587;
                        } else {
                            return 5.441672235835747;
                        }
                    }
                } else {
                    if (((double) p.stage0FullY104 / (p.stage0FullY96 + EPSILON)) <= 0.8204035523363549) {
                        if ((p.stage0Y88LargestCluster + p.stage0Y96LargestCluster) <= 14.2989008381492) {
                            return 57.12820512820513;
                        } else {
                            return 19.85144927536232;
                        }
                    } else {
                        if (p.stage0Y88Depth <= 5.578773481575201) {
                            return 7.146077547339946;
                        } else {
                            return 12.547448680351906;
                        }
                    }
                }
            }
        }
    }

    private static double tree2(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        if (p.stage0Y96Depth <= 6.226280006685425) {
            if (p.stage0Y96LargestCluster <= 2.8740906477319497) {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.8263983023772782) {
                    if (p.stage0FullY88 <= 5.881502184496029) {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 9.84651081722533) {
                            return 2.0603032649696487;
                        } else {
                            return 3.312460568045968;
                        }
                    } else {
                        if (p.stage0FullY88 <= 9.238529731164933) {
                            return 3.7259671897704805;
                        } else {
                            return 5.811142828409416;
                        }
                    }
                } else {
                    if ((p.stage0Y96Width * p.stage0Y96Depth) <= 7.660911924979143) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.949721036384572) {
                            return 4.59908948641343;
                        } else {
                            return 8.67107920125645;
                        }
                    } else {
                        if (p.stage0Y88LargestCluster <= 5.132168327519748) {
                            return 11.905877154220061;
                        } else {
                            return 74.16;
                        }
                    }
                }
            } else {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.6909630940658062) {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.41900917304653607) {
                        if (p.stage0Y96Width <= 5.920127410404444) {
                            return 3.494586109766842;
                        } else {
                            return 4.583359217526352;
                        }
                    } else {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 26.320397817213273) {
                            return 5.146835243165208;
                        } else {
                            return 10.153347616497054;
                        }
                    }
                } else {
                    if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.7309737665732358) {
                        if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.3732714650822998) {
                            return 64.87878787878788;
                        } else {
                            return 9.911308709530324;
                        }
                    } else {
                        if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.9002993950598424) {
                            return 7.236054168219654;
                        } else {
                            return 4.46419820310373;
                        }
                    }
                }
            }
        } else {
            if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.49448978349537104) {
                if (p.stage0FullY96 <= 10.993057597890989) {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.27078133844957053) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.23611566525625793) {
                            return 3.9821239072906462;
                        } else {
                            return 5.2729389994895355;
                        }
                    } else {
                        if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.36312086142266037) {
                            return 6.869978651630461;
                        } else {
                            return 5.560418848167539;
                        }
                    }
                } else {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.22151932757395038) {
                        if ((p.stage0Y96Width * p.stage0Y96Depth) <= 121.89213091968192) {
                            return 4.4997953336062215;
                        } else {
                            return 5.7290989832885355;
                        }
                    } else {
                        if ((p.stage0Y96Width * p.stage0Y96Depth) <= 89.61466998169882) {
                            return 6.276031262047454;
                        } else {
                            return 7.582176308733806;
                        }
                    }
                }
            } else {
                if (p.stage0FullY88 <= 11.184627735141957) {
                    if (p.stage0Y88Width <= 4.272638146892487) {
                        if (p.stage0Y96LargestCluster <= 5.13589972077678) {
                            return 5.410653816097942;
                        } else {
                            return 9.503802281368822;
                        }
                    } else {
                        if ((p.stage0FullY96 + p.stage0FullY104 + p.stage0FullY112) <= 19.620677768076153) {
                            return 8.485278836162106;
                        } else {
                            return 31.41860465116279;
                        }
                    }
                } else {
                    if (p.stage0FullY112 <= 13.494788573353919) {
                        if (p.stage0FullY112 <= 9.58063563498698) {
                            return 11.93706975676916;
                        } else {
                            return 6.027272727272727;
                        }
                    } else {
                        return 74.26666666666667;
                    }
                }
            }
        }
    }

    private static double tree3(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        if (p.stage0FullY88 <= 7.951088068518363) {
            if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.32129693232570883) {
                if (p.stage0FullY96 <= 3.795354517081452) {
                    if (scout <= 56.72173221442929) {
                        if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.3781256500362621) {
                            return 3.5209722337253604;
                        } else {
                            return 3.1145247599391968;
                        }
                    } else {
                        if (p.stage0FullY104 <= 0.5601421960443921) {
                            return 2.3297135544096057;
                        } else {
                            return 2.8137924772433207;
                        }
                    }
                } else {
                    if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 30.789549501514784) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.09577607569019495) {
                            return 2.717498268325889;
                        } else {
                            return 3.4015885837372104;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.1620905919908936) {
                            return 3.666403361387782;
                        } else {
                            return 4.4785405850638975;
                        }
                    }
                }
            } else {
                if (p.stage0Y96Depth <= 2.7268893135371104) {
                    if (p.stage0FullY88 <= 5.366241472012476) {
                        if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.7138644308837625) {
                            return 4.172751688560336;
                        } else {
                            return 2.8099466705807523;
                        }
                    } else {
                        if (p.stage0Y88Depth <= 5.870728385574523) {
                            return 4.134332105791764;
                        } else {
                            return 5.5390631125049;
                        }
                    }
                } else {
                    if (p.stage0Y88Width <= 4.404597487954701) {
                        if (scout <= 11.91112454557378) {
                            return 5.306594339050852;
                        } else {
                            return 3.666377950320166;
                        }
                    } else {
                        if (scout <= 11.684303294254608) {
                            return 7.814258911819888;
                        } else {
                            return 5.827160114030231;
                        }
                    }
                }
            }
        } else {
            if (scout <= 43.695221512902165) {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.5117404946304966) {
                    if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.38972985392326437) {
                        if (p.stage0FullY96 <= 7.132104213876699) {
                            return 5.326365744365162;
                        } else {
                            return 7.630434025964986;
                        }
                    } else {
                        if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.5098613509750699) {
                            return 5.7266353637357685;
                        } else {
                            return 4.574153904780051;
                        }
                    }
                } else {
                    if (p.stage0Y96Depth <= 5.468862200839847) {
                        if (scout <= 24.61358558880798) {
                            return 6.72658972344754;
                        } else {
                            return 10.735004224162207;
                        }
                    } else {
                        if (scout <= 10.946943884225071) {
                            return 31.426865671641792;
                        } else {
                            return 10.028997841879692;
                        }
                    }
                }
            } else {
                if (p.stage0FullY96 <= 10.20230213655266) {
                    if (scout <= 89.10415656822927) {
                        if (p.stage0FullY88 <= 19.016875931535324) {
                            return 4.5068630277404065;
                        } else {
                            return 9.3715953307393;
                        }
                    } else {
                        if (p.stage0FullY88 <= 15.90620878986372) {
                            return 3.296625781511001;
                        } else {
                            return 4.952688547486034;
                        }
                    }
                } else {
                    if ((p.stage0Y88Width * p.stage0Y88Depth) <= 99.27091870795512) {
                        if (((double) p.stage0FullY104 / (p.stage0FullY96 + EPSILON)) <= 0.2620582220696862) {
                            return 3.6633221850613156;
                        } else {
                            return 5.1141860343108965;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.5022581448178246) {
                            return 6.492265410886907;
                        } else {
                            return 13.563660477453581;
                        }
                    }
                }
            }
        }
    }

    private static double tree4(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.13886295468698584) {
            if ((p.stage0Y96Width * p.stage0Y96Depth) <= 88.16770903401435) {
                if ((p.stage0Y88Width * p.stage0Y88Depth) <= 23.68186700888065) {
                    if ((p.stage0Y96Width * p.stage0Y96Depth) <= 10.088270773220934) {
                        if (p.stage0Y88Width <= 4.381605378064143) {
                            return 2.2578719594090435;
                        } else {
                            return 1.4912296564195298;
                        }
                    } else {
                        if (((double) p.stage0Y96LargestCluster / (p.stage0FullY96 + EPSILON)) <= 0.5869459719591159) {
                            return 3.487558282208589;
                        } else {
                            return 2.4273078360533202;
                        }
                    }
                } else {
                    if (p.stage0Y96Width <= 3.9116333666777394) {
                        if (p.stage0Y88Depth <= 10.044414547292709) {
                            return 2.800037642698402;
                        } else {
                            return 3.167538446468802;
                        }
                    } else {
                        if (p.stage0FullY96 <= 5.26404633152487) {
                            return 3.1277502604828893;
                        } else {
                            return 3.511416912399845;
                        }
                    }
                }
            } else {
                if (p.stage0FullY96 <= 5.201951116417511) {
                    if (p.stage0FullY96 <= 3.773854051145657) {
                        if (p.stage0Y96Width <= 9.529296705280103) {
                            return 3.594072743601257;
                        } else {
                            return 2.0266299833887045;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.09074507213263047) {
                            return 2.9692947253606636;
                        } else {
                            return 3.993309915376018;
                        }
                    }
                } else {
                    if (p.stage0FullY88 <= 8.218338548307464) {
                        if (p.stage0Y88Depth <= 9.156840975003709) {
                            return 2.7027530531980957;
                        } else {
                            return 4.018965270147614;
                        }
                    } else {
                        if (p.stage0FullY96 <= 11.586174790554761) {
                            return 4.373558724739827;
                        } else {
                            return 5.244808055380743;
                        }
                    }
                }
            }
        } else {
            if ((p.stage0Y96Width * p.stage0Y96Depth) <= 83.11893896158735) {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.44930952223117077) {
                    if ((p.stage0Y96Width * p.stage0Y96Depth) <= 13.999006981029645) {
                        if ((p.stage0Y88Width * p.stage0Y88Depth) <= 34.718240714035105) {
                            return 2.9905253851842377;
                        } else {
                            return 3.917124686204172;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.29533617868264656) {
                            return 4.197281565996776;
                        } else {
                            return 5.887307347292157;
                        }
                    }
                } else {
                    if (p.stage0Y88Width <= 5.2442442100701125) {
                        if (p.stage0Y96Width <= 4.994701263492322) {
                            return 4.6501570364140505;
                        } else {
                            return 8.583820873961718;
                        }
                    } else {
                        if (p.stage0Y96Width <= 3.4158589930347443) {
                            return 5.8755432192222505;
                        } else {
                            return 8.446920435449696;
                        }
                    }
                }
            } else {
                if (p.stage0FullY88 <= 14.711982085235409) {
                    if (p.stage0FullY96 <= 9.211285633599054) {
                        if (p.stage0FullY96 <= 6.400494685117386) {
                            return 4.70381902882469;
                        } else {
                            return 5.524887038395117;
                        }
                    } else {
                        if (scout <= 45.83925156753475) {
                            return 7.403867075462368;
                        } else {
                            return 5.521449862283206;
                        }
                    }
                } else {
                    if ((p.stage0FullY96 + p.stage0FullY104 + p.stage0FullY112) <= 32.241552607268815) {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 50.12080392247103) {
                            return 6.960521807234813;
                        } else {
                            return 6.1604221635883905;
                        }
                    } else {
                        if (scout <= 68.14262678341132) {
                            return 10.243278378538085;
                        } else {
                            return 7.392949937601014;
                        }
                    }
                }
            }
        }
    }

    private static double tree5(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        if (p.stage0FullY88 <= 7.6830480018086025) {
            if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.8111430084960018) {
                if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 15.318402108884603) {
                    if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 8.069986446327523) {
                        if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.4586456433219546) {
                            return 3.1689851767388824;
                        } else {
                            return 2.000569719413189;
                        }
                    } else {
                        if (((double) p.stage0Y96LargestCluster / (p.stage0FullY96 + EPSILON)) <= 0.03409658572886586) {
                            return 3.8449556138685725;
                        } else {
                            return 3.0405683056226613;
                        }
                    }
                } else {
                    if (scout <= 62.832070299346036) {
                        if (scout <= 16.66490496865702) {
                            return 5.668116395666189;
                        } else {
                            return 3.6267391999403644;
                        }
                    } else {
                        if ((p.stage0FullY96 + p.stage0FullY104 + p.stage0FullY112) <= 5.747864221105289) {
                            return 2.7335847467360925;
                        } else {
                            return 3.229103650274421;
                        }
                    }
                }
            } else {
                if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.36474720127733234) {
                    if (((double) p.stage0FullY96 / (p.stage0FullY88 + EPSILON)) <= 0.5470067809863269) {
                        if (((double) p.stage0Y96LargestCluster / (p.stage0FullY96 + EPSILON)) <= 0.5571745638782855) {
                            return 21.939670932358318;
                        } else {
                            return 44.06666666666667;
                        }
                    } else {
                        return 5.892733564013841;
                    }
                } else {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.9171767077476005) {
                        if ((p.stage0Y88LargestCluster + p.stage0Y96LargestCluster) <= 7.574976491543919) {
                            return 4.5366008911521325;
                        } else {
                            return 7.945847054518843;
                        }
                    } else {
                        if (p.stage0Y88Width <= 8.43677356161027) {
                            return 7.624549185424699;
                        } else {
                            return 52.6875;
                        }
                    }
                }
            }
        } else {
            if (p.stage0FullY96 <= 13.3480104679931) {
                if (p.stage0FullY96 <= 8.888775560487423) {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.23687700209273518) {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 34.36315160587577) {
                            return 3.80119624242511;
                        } else {
                            return 4.383957604586148;
                        }
                    } else {
                        if ((p.stage0Y96Width * p.stage0Y96Depth) <= 52.29081454327834) {
                            return 5.354807717968562;
                        } else {
                            return 6.705549760862929;
                        }
                    }
                } else {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.5052130682282449) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.3106718277716015) {
                            return 4.9125716552754675;
                        } else {
                            return 7.451637253122104;
                        }
                    } else {
                        if ((p.stage0FullY96 + p.stage0FullY104 + p.stage0FullY112) <= 20.658367571261884) {
                            return 8.176053884566253;
                        } else {
                            return 12.285786163522012;
                        }
                    }
                }
            } else {
                if ((p.stage0Y88Width * p.stage0Y88Depth) <= 232.76067277824538) {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.4717097037929805) {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 46.451284115412875) {
                            return 6.681022506251737;
                        } else {
                            return 7.747202485049346;
                        }
                    } else {
                        if (p.stage0Y88Depth <= 6.317052728644131) {
                            return 5.65929941618015;
                        } else {
                            return 13.457847106453917;
                        }
                    }
                } else {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.5209377455089925) {
                        if (p.stage0FullY88 <= 20.8877554055712) {
                            return 4.414702006150241;
                        } else {
                            return 6.618213841690841;
                        }
                    } else {
                        return 72.0;
                    }
                }
            }
        }
    }

    private static double tree6(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        if (p.stage0FullY104 <= 8.786006483149718) {
            if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.36947649705421315) {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.19216483106348836) {
                    if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 27.6850069163763) {
                        if (p.stage0FullY88 <= 6.470630214303094) {
                            return 2.8688472384246158;
                        } else {
                            return 3.4318156133455555;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.11088241633952733) {
                            return 3.2366063834642302;
                        } else {
                            return 4.088210664639724;
                        }
                    }
                } else {
                    if (p.stage0Y88Width <= 7.972405306050742) {
                        if (p.stage0Y96Depth <= 4.227238128067571) {
                            return 3.362258224721816;
                        } else {
                            return 4.36093399251294;
                        }
                    } else {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 41.97347354142623) {
                            return 4.655750504891251;
                        } else {
                            return 5.650997838673545;
                        }
                    }
                }
            } else {
                if (p.stage0Y96Depth <= 4.534662822510424) {
                    if (p.stage0Y88Width <= 4.928496716045075) {
                        if (p.stage0Y88Depth <= 4.68029374009012) {
                            return 3.4523200190050427;
                        } else {
                            return 4.67772797620244;
                        }
                    } else {
                        if ((p.stage0Y96Width * p.stage0Y96Depth) <= 10.973086243017152) {
                            return 5.239073795227771;
                        } else {
                            return 7.245239601909912;
                        }
                    }
                } else {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.8005579335455628) {
                        if ((p.stage0Y88Width * p.stage0Y88Depth) <= 65.89796568308861) {
                            return 5.893568139264207;
                        } else {
                            return 7.94490936901141;
                        }
                    } else {
                        if (((double) p.stage0FullY96 / (p.stage0FullY88 + EPSILON)) <= 0.6296772525360232) {
                            return 19.83359497645212;
                        } else {
                            return 10.50442198357549;
                        }
                    }
                }
            }
        } else {
            if (p.stage0Y96Depth <= 11.011482299776507) {
                if (p.stage0FullY96 <= 12.797917784563227) {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.4518888209378756) {
                        if (((double) p.stage0FullY96 / (p.stage0FullY88 + EPSILON)) <= 0.9181565483463612) {
                            return 6.344648680699674;
                        } else {
                            return 4.740923755462082;
                        }
                    } else {
                        if (((double) p.stage0Y96LargestCluster / (p.stage0FullY96 + EPSILON)) <= 0.4403897162544709) {
                            return 57.340425531914896;
                        } else {
                            return 9.217904961932266;
                        }
                    }
                } else {
                    if (scout <= 65.55681558072419) {
                        if ((p.stage0Y96Width * p.stage0Y96Depth) <= 104.59773775590479) {
                            return 6.901898734177215;
                        } else {
                            return 9.741331308529487;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.25069665767024474) {
                            return 5.566516545917368;
                        } else {
                            return 6.917382093878916;
                        }
                    }
                }
            } else {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.5850489314215824) {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.4787680376493918) {
                        if (p.stage0Y96Depth <= 15.4597571843582) {
                            return 7.363506450545815;
                        } else {
                            return 5.585555313113681;
                        }
                    } else {
                        if ((p.stage0Y88LargestCluster + p.stage0Y96LargestCluster) <= 10.25200109568052) {
                            return 4.446913580246914;
                        } else {
                            return 19.93143245078072;
                        }
                    }
                } else {
                    if (scout <= 30.12059765640674) {
                        if (p.stage0FullY112 <= 3.6949989212187933) {
                            return 64.38095238095238;
                        } else {
                            return 16.794007490636705;
                        }
                    } else {
                        if (p.stage0Y88Depth <= 14.417349926390383) {
                            return 56.0;
                        } else {
                            return 80.32432432432432;
                        }
                    }
                }
            }
        }
    }

    private static double tree7(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.39178434996465333) {
            if (p.stage0FullY88 <= 8.884601466303971) {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.10104890552941961) {
                    if (p.stage0FullY88 <= 6.767994009781626) {
                        if (p.stage0Y96Depth <= 4.437428282777512) {
                            return 2.5175783201998847;
                        } else {
                            return 3.016831383321319;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.06875965752805682) {
                            return 2.2969259831701874;
                        } else {
                            return 3.546624426480227;
                        }
                    }
                } else {
                    if (p.stage0Y96Depth <= 6.603328454589159) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.2513242664383716) {
                            return 3.3007904604349187;
                        } else {
                            return 3.953775281964009;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.21066525111874917) {
                            return 3.93841769016238;
                        } else {
                            return 4.890173707208618;
                        }
                    }
                }
            } else {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.23260688060518556) {
                    if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 31.72326762681979) {
                        if (p.stage0FullY88 <= 13.801566025615468) {
                            return 3.6767841205830827;
                        } else {
                            return 8.062025316455696;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.19455476316717885) {
                            return 4.416048143116834;
                        } else {
                            return 5.445513785539163;
                        }
                    }
                } else {
                    if ((p.stage0Y96Width * p.stage0Y96Depth) <= 26.346045267929345) {
                        if ((p.stage0FullY96 + p.stage0FullY104 + p.stage0FullY112) <= 12.308379566440536) {
                            return 4.991816474376174;
                        } else {
                            return 3.6983279261292736;
                        }
                    } else {
                        if (p.stage0Y96Width <= 7.756484157292398) {
                            return 5.640185957976426;
                        } else {
                            return 6.661286112361897;
                        }
                    }
                }
            }
        } else {
            if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 16.24571259610621) {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.7237550078092878) {
                    if ((p.stage0Y88Width * p.stage0Y88Depth) <= 21.833724272825627) {
                        if (p.stage0Y96Width <= 2.2346424405624354) {
                            return 2.7661840715040404;
                        } else {
                            return 4.26864872103924;
                        }
                    } else {
                        if (((double) p.stage0Y96LargestCluster / (p.stage0FullY96 + EPSILON)) <= 0.340983407594832) {
                            return 7.526694108250906;
                        } else {
                            return 5.20821140374213;
                        }
                    }
                } else {
                    if (p.stage0Y88LargestCluster <= 5.227648453892976) {
                        if ((p.stage0Y96Width * p.stage0Y96Depth) <= 5.568054686099316) {
                            return 5.922744503411676;
                        } else {
                            return 10.028395484091687;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.7816728868467179) {
                            return 3.3600995249943453;
                        } else {
                            return 5.092326139088729;
                        }
                    }
                }
            } else {
                if ((p.stage0Y96Width * p.stage0Y96Depth) <= 40.002017993480905) {
                    if (p.stage0Y96Width <= 4.72630777430083) {
                        if ((p.stage0Y88LargestCluster + p.stage0Y96LargestCluster) <= 4.3286517752933555) {
                            return 5.018053542089494;
                        } else {
                            return 6.167517150484945;
                        }
                    } else {
                        if (p.stage0Y88Depth <= 6.063423323474556) {
                            return 7.066056324012339;
                        } else {
                            return 9.319666975023127;
                        }
                    }
                } else {
                    if (((double) p.stage0FullY104 / (p.stage0FullY96 + EPSILON)) <= 0.43268891118126407) {
                        if ((p.stage0FullY96 + p.stage0FullY104 + p.stage0FullY112) <= 7.751867331448881) {
                            return 6.524640698186476;
                        } else {
                            return 8.646232439335888;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.9363761879419367) {
                            return 9.168071003258062;
                        } else {
                            return 67.5;
                        }
                    }
                }
            }
        }
    }

    private static double tree8(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        if (p.stage0FullY96 <= 8.120488984566409) {
            if (p.stage0FullY88 <= 10.501895916881923) {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.1196530855082352) {
                    if ((p.stage0Y88Width * p.stage0Y88Depth) <= 58.33447384197939) {
                        if ((p.stage0Y96Width * p.stage0Y96Depth) <= 51.55009883434982) {
                            return 2.644891779991187;
                        } else {
                            return 3.859348753068872;
                        }
                    } else {
                        if ((p.stage0FullY96 + p.stage0FullY104 + p.stage0FullY112) <= 4.87108470353814) {
                            return 2.7426214170281535;
                        } else {
                            return 3.4436938910634654;
                        }
                    }
                } else {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.43298923184639804) {
                        if (p.stage0FullY96 <= 3.249887117766723) {
                            return 3.5784900113353326;
                        } else {
                            return 4.204909422130759;
                        }
                    } else {
                        if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.5455802348262206) {
                            return 6.601934801542864;
                        } else {
                            return 4.687799027800814;
                        }
                    }
                }
            } else {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.4931276379383832) {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.35505177868692744) {
                        if (((double) p.stage0FullY96 / (p.stage0FullY88 + EPSILON)) <= 0.019441139641402767) {
                            return 9.150227617602427;
                        } else {
                            return 5.004734872809158;
                        }
                    } else {
                        if ((p.stage0Y88Width * p.stage0Y88Depth) <= 44.66599383049923) {
                            return 4.384817211824384;
                        } else {
                            return 7.123937288600421;
                        }
                    }
                } else {
                    if (((double) p.stage0FullY104 / (p.stage0FullY96 + EPSILON)) <= 0.3694147964935823) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.8272656877232956) {
                            return 8.421534408956207;
                        } else {
                            return 19.96604938271605;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.8417231909004731) {
                            return 12.437535330695308;
                        } else {
                            return 78.63333333333334;
                        }
                    }
                }
            }
        } else {
            if (p.stage0FullY88 <= 20.735934806864677) {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.3443502457889277) {
                    if ((p.stage0Y88Width * p.stage0Y88Depth) <= 100.54293362821589) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.240065176176856) {
                            return 4.019493706270708;
                        } else {
                            return 5.420972538450815;
                        }
                    } else {
                        if (p.stage0FullY96 <= 14.763216681202508) {
                            return 5.526315603138188;
                        } else {
                            return 7.145160913906976;
                        }
                    }
                } else {
                    if (p.stage0Y88LargestCluster <= 3.05168063475363) {
                        if ((p.stage0Y88LargestCluster + p.stage0Y96LargestCluster) <= 4.174742448225032) {
                            return 8.182608695652174;
                        } else {
                            return 11.248742694032893;
                        }
                    } else {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 31.296954857789423) {
                            return 7.1551286781662204;
                        } else {
                            return 8.875602550147722;
                        }
                    }
                }
            } else {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.4170299130271343) {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.19702122994005733) {
                        if (p.stage0Y88Depth <= 12.035476170962433) {
                            return 6.783206106870229;
                        } else {
                            return 4.221574344023324;
                        }
                    } else {
                        if (((double) p.stage0FullY96 / (p.stage0FullY88 + EPSILON)) <= 0.3969451929047156) {
                            return 48.40909090909091;
                        } else {
                            return 7.369244819550293;
                        }
                    }
                } else {
                    if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.6147290178796001) {
                        if (scout <= 55.24903128447431) {
                            return 12.863221884498481;
                        } else {
                            return 8.037397260273973;
                        }
                    } else {
                        if (scout <= 38.240017077531675) {
                            return 8.3625;
                        } else {
                            return 4.6702311141773025;
                        }
                    }
                }
            }
        }
    }

    private static double tree9(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        if ((p.stage0Y96Width * p.stage0Y96Depth) <= 60.34623578020662) {
            if (scout <= 43.22855165145758) {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.41007262085505886) {
                    if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 26.358242123088914) {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 17.346914509938525) {
                            return 3.1501489413090735;
                        } else {
                            return 3.705911746517168;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.17121771921149903) {
                            return 3.6729043329747837;
                        } else {
                            return 4.853525001656647;
                        }
                    }
                } else {
                    if ((p.stage0Y96Width * p.stage0Y96Depth) <= 21.273787897673337) {
                        if ((p.stage0Y96Width * p.stage0Y96Depth) <= 6.51718003466594) {
                            return 4.618377734519326;
                        } else {
                            return 5.932982089766377;
                        }
                    } else {
                        if (p.stage0FullY88 <= 21.599372770450717) {
                            return 7.901229568132474;
                        } else {
                            return 69.53333333333333;
                        }
                    }
                }
            } else {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.18084453548352333) {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.1245935275986041) {
                        if ((p.stage0Y96Width * p.stage0Y96Depth) <= 42.750965386047476) {
                            return 2.723866746913316;
                        } else {
                            return 3.614328079986443;
                        }
                    } else {
                        if (p.stage0FullY96 <= 1.5166889007746656) {
                            return 2.7582854012599287;
                        } else {
                            return 3.685710322719847;
                        }
                    }
                } else {
                    if (p.stage0FullY88 <= 16.596724815944548) {
                        if (p.stage0Y88Width <= 9.55637323882262) {
                            return 4.0779657250784656;
                        } else {
                            return 5.112718482707326;
                        }
                    } else {
                        if (((double) p.stage0FullY104 / (p.stage0FullY96 + EPSILON)) <= 0.9848406184116537) {
                            return 7.010174843576399;
                        } else {
                            return 12.103467253714914;
                        }
                    }
                }
            }
        } else {
            if (p.stage0FullY96 <= 21.212675386673155) {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.25692606057545786) {
                    if (p.stage0FullY96 <= 8.584570717338739) {
                        if (p.stage0Y96LargestCluster <= 1.4907999962991103) {
                            return 3.5557454706909386;
                        } else {
                            return 4.332474838631884;
                        }
                    } else {
                        if (p.stage0FullY96 <= 12.402029320319198) {
                            return 4.858510084662935;
                        } else {
                            return 6.044977690717162;
                        }
                    }
                } else {
                    if (p.stage0FullY88 <= 14.730136585761857) {
                        if (p.stage0FullY96 <= 5.118580702281827) {
                            return 5.583424107973113;
                        } else {
                            return 7.040682089682702;
                        }
                    } else {
                        if (scout <= 38.359732929784684) {
                            return 9.25559462915601;
                        } else {
                            return 7.38490409380507;
                        }
                    }
                }
            } else {
                if ((p.stage0Y88Width * p.stage0Y88Depth) <= 86.56738367004664) {
                    return 73.65517241379311;
                } else {
                    if ((p.stage0FullY96 + p.stage0FullY104 + p.stage0FullY112) <= 68.94068289219433) {
                        if (p.stage0Y96LargestCluster <= 9.975565975520558) {
                            return 8.828214971209214;
                        } else {
                            return 10.98095909732017;
                        }
                    } else {
                        if (scout <= 106.87444658385331) {
                            return 6.790064102564102;
                        } else {
                            return 3.5843801201529217;
                        }
                    }
                }
            }
        }
    }

    private static double tree10(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        if ((p.stage0Y96Width * p.stage0Y96Depth) <= 52.76896479202852) {
            if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.2832229906687228) {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.12471700735594299) {
                    if ((p.stage0Y96Width * p.stage0Y96Depth) <= 36.341204516748526) {
                        if (p.stage0Y88Depth <= 3.000332885887629) {
                            return 2.0572076913889883;
                        } else {
                            return 2.8036163599585575;
                        }
                    } else {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 35.25628533133133) {
                            return 2.9350606349074955;
                        } else {
                            return 3.896527108541243;
                        }
                    }
                } else {
                    if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 20.216146265722877) {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 13.871363489363047) {
                            return 2.5436780206680996;
                        } else {
                            return 3.304789710446706;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.177049933337199) {
                            return 3.4517704461181657;
                        } else {
                            return 4.059605052749804;
                        }
                    }
                }
            } else {
                if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 27.333515604181922) {
                    if (p.stage0Y88Width <= 4.645824477377266) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.9923554765190882) {
                            return 3.7415567590185965;
                        } else {
                            return 7.657964912280701;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.4002976635827121) {
                            return 4.479174839679476;
                        } else {
                            return 6.146624578369219;
                        }
                    }
                } else {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.7662168588780214) {
                        if ((p.stage0FullY96 + p.stage0FullY104 + p.stage0FullY112) <= 3.524479415977597) {
                            return 5.282795825026387;
                        } else {
                            return 6.9414586133697656;
                        }
                    } else {
                        if ((p.stage0Y88Width * p.stage0Y88Depth) <= 124.53162607761641) {
                            return 22.5385450597177;
                        } else {
                            return 6.996354799513973;
                        }
                    }
                }
            }
        } else {
            if (p.stage0FullY88 <= 27.639932110272316) {
                if (p.stage0FullY96 <= 12.866914973997938) {
                    if (p.stage0FullY88 <= 11.58912873016164) {
                        if (p.stage0FullY88 <= 6.98720098938197) {
                            return 3.7498222912976953;
                        } else {
                            return 4.818514514898797;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.531000897651182) {
                            return 5.603268876041447;
                        } else {
                            return 8.6934915631374;
                        }
                    }
                } else {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.24397623866538443) {
                        if (p.stage0Y96Depth <= 6.596319251971468) {
                            return 2.613937282229965;
                        } else {
                            return 6.092289348171701;
                        }
                    } else {
                        if ((p.stage0FullY96 + p.stage0FullY104 + p.stage0FullY112) <= 37.81722842846868) {
                            return 7.466595824906592;
                        } else {
                            return 9.161033059767236;
                        }
                    }
                }
            } else {
                if ((p.stage0FullY96 + p.stage0FullY104 + p.stage0FullY112) <= 70.87567508540846) {
                    if (scout <= 47.0353787122459) {
                        return 20.269896193771626;
                    } else {
                        if ((p.stage0Y88Width * p.stage0Y88Depth) <= 139.81619919641696) {
                            return 6.370581527936146;
                        } else {
                            return 8.971833020366892;
                        }
                    }
                } else {
                    if ((p.stage0Y96Width * p.stage0Y96Depth) <= 208.06746707068174) {
                        if ((p.stage0FullY96 + p.stage0FullY104 + p.stage0FullY112) <= 75.21545557594646) {
                            return 10.25515947467167;
                        } else {
                            return 4.6105092091007585;
                        }
                    } else {
                        return 3.1801007556675063;
                    }
                }
            }
        }
    }

    private static double tree11(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.26166783451522496) {
            if (p.stage0FullY88 <= 12.262184602330525) {
                if (p.stage0FullY96 <= 6.302120444505531) {
                    if ((p.stage0Y88Width * p.stage0Y88Depth) <= 86.62234187062008) {
                        if ((p.stage0Y96Width * p.stage0Y96Depth) <= 25.635324480773566) {
                            return 3.0230655320345345;
                        } else {
                            return 3.411729875196152;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.17718427124102934) {
                            return 3.580259926204274;
                        } else {
                            return 4.247655564127803;
                        }
                    }
                } else {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.10101345335932503) {
                        if ((p.stage0Y96Width * p.stage0Y96Depth) <= 163.43764296939486) {
                            return 3.3926522593320234;
                        } else {
                            return 4.590532985464033;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.11535588185226384) {
                            return 3.8000304522153985;
                        } else {
                            return 4.700103854715674;
                        }
                    }
                }
            } else {
                if (p.stage0FullY96 <= 16.836475497501194) {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.15160882139678344) {
                        if ((p.stage0Y96Width * p.stage0Y96Depth) <= 200.99275229172443) {
                            return 4.063013698630137;
                        } else {
                            return 5.000399627014787;
                        }
                    } else {
                        if (p.stage0FullY96 <= 13.047038486714959) {
                            return 5.25328160151283;
                        } else {
                            return 6.1943159613148975;
                        }
                    }
                } else {
                    if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 55.73456004994668) {
                        if (p.stage0Y96Width <= 14.03214175907529) {
                            return 8.354914196567863;
                        } else {
                            return 4.90558316281789;
                        }
                    } else {
                        if ((p.stage0Y88Width * p.stage0Y88Depth) <= 209.2509476622804) {
                            return 6.921766743648961;
                        } else {
                            return 5.60121849903074;
                        }
                    }
                }
            }
        } else {
            if ((p.stage0Y88Width * p.stage0Y88Depth) <= 53.26725613245147) {
                if ((p.stage0Y96Width * p.stage0Y96Depth) <= 24.337004151352218) {
                    if (p.stage0Y88Width <= 4.954538262428035) {
                        if (p.stage0Y96Width <= 2.677046348656084) {
                            return 3.2691005771350756;
                        } else {
                            return 4.379047187886529;
                        }
                    } else {
                        if (p.stage0Y88Depth <= 3.0699877213081286) {
                            return 3.756420213316765;
                        } else {
                            return 5.011146018742896;
                        }
                    }
                } else {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.6848830060593631) {
                        if (scout <= 10.768805123292285) {
                            return 10.6189948263119;
                        } else {
                            return 5.5480963131382115;
                        }
                    } else {
                        if ((p.stage0Y88Width * p.stage0Y88Depth) <= 47.155815965568564) {
                            return 21.460748792270532;
                        } else {
                            return 7.699052132701421;
                        }
                    }
                }
            } else {
                if (p.stage0FullY104 <= 8.446497096888022) {
                    if (p.stage0Y96Width <= 4.94238547465875) {
                        if (p.stage0Y96Depth <= 5.220299566732859) {
                            return 5.00268548338701;
                        } else {
                            return 6.672963315864697;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.5314852831270693) {
                            return 6.4163976324993275;
                        } else {
                            return 9.491876194677253;
                        }
                    }
                } else {
                    if (p.stage0FullY112 <= 1.5769117365483716) {
                        if (scout <= 37.89041354595237) {
                            return 14.754208754208754;
                        } else {
                            return 8.788672153457327;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.7652088777852346) {
                            return 7.914191744373683;
                        } else {
                            return 83.5925925925926;
                        }
                    }
                }
            }
        }
    }

    private static double tree12(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        if (p.stage0FullY88 <= 12.756130358090685) {
            if (p.stage0FullY88 <= 7.384111282128893) {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.8180572652643205) {
                    if (p.stage0FullY96 <= 4.717749285380239) {
                        if (p.stage0FullY88 <= 6.642645475288222) {
                            return 3.380397307177002;
                        } else {
                            return 3.9186679485937996;
                        }
                    } else {
                        if (p.stage0FullY96 <= 5.415531712777694) {
                            return 3.6641289364860565;
                        } else {
                            return 4.246143938813663;
                        }
                    }
                } else {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.9783944945227121) {
                        if (p.stage0Y96Depth <= 2.9097566692669674) {
                            return 4.2792575629745695;
                        } else {
                            return 8.328445747800586;
                        }
                    } else {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 24.80001377537847) {
                            return 8.175263915547024;
                        } else {
                            return 57.666666666666664;
                        }
                    }
                }
            } else {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.37665751528343133) {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.27158463651120457) {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 30.008083087395914) {
                            return 3.7602665699453013;
                        } else {
                            return 4.473081579309396;
                        }
                    } else {
                        if ((p.stage0Y88Width * p.stage0Y88Depth) <= 76.07150272748969) {
                            return 4.77872098242914;
                        } else {
                            return 6.090044352922541;
                        }
                    }
                } else {
                    if (p.stage0FullY112 <= 7.917752910367098) {
                        if (p.stage0FullY96 <= 7.979672668492269) {
                            return 6.177960720279403;
                        } else {
                            return 8.190893739954834;
                        }
                    } else {
                        return 78.46666666666667;
                    }
                }
            }
        } else {
            if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.3053133004654409) {
                if (p.stage0Y88Depth <= 8.369706768249024) {
                    if (p.stage0FullY104 <= 8.920516620818766) {
                        if (p.stage0Y88Width <= 11.189882956533637) {
                            return 4.2883457005674375;
                        } else {
                            return 6.210568312793687;
                        }
                    } else {
                        if ((p.stage0FullY96 + p.stage0FullY104 + p.stage0FullY112) <= 26.67250656693825) {
                            return 3.130358705161855;
                        } else {
                            return 4.256336111583603;
                        }
                    }
                } else {
                    if (p.stage0FullY96 <= 23.072196600526663) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.1513719585993001) {
                            return 4.397499591817694;
                        } else {
                            return 5.914674480456634;
                        }
                    } else {
                        if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.1821002328979785) {
                            return 3.211394302848576;
                        } else {
                            return 10.902391219129752;
                        }
                    }
                }
            } else {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.5608946907128665) {
                    if (((double) p.stage0Y96LargestCluster / (p.stage0FullY96 + EPSILON)) <= 0.4722589282426576) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.4857895679703196) {
                            return 8.36015088870892;
                        } else {
                            return 11.446244284781189;
                        }
                    } else {
                        if ((p.stage0Y88Width * p.stage0Y88Depth) <= 134.25133236788741) {
                            return 6.116114913428737;
                        } else {
                            return 7.981313790192617;
                        }
                    }
                } else {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.8598375173988544) {
                        if ((p.stage0Y88Width * p.stage0Y88Depth) <= 54.23661620832822) {
                            return 6.8108015723807895;
                        } else {
                            return 13.348609003540718;
                        }
                    } else {
                        if (p.stage0FullY96 <= 9.584528189890333) {
                            return 68.47368421052632;
                        } else {
                            return 88.38636363636364;
                        }
                    }
                }
            }
        }
    }

    private static double tree13(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        if (p.stage0Y96Width <= 5.549413571855712) {
            if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.47247860712949835) {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.1741215600561226) {
                    if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 16.231616748139082) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.07759969539511571) {
                            return 1.6417540964281505;
                        } else {
                            return 2.6880317100400704;
                        }
                    } else {
                        if (p.stage0Y88Width <= 10.092982839370604) {
                            return 3.0722759989015223;
                        } else {
                            return 3.417632669848795;
                        }
                    }
                } else {
                    if (p.stage0Y88Depth <= 6.710309015452074) {
                        if (p.stage0Y88Width <= 7.754588064157677) {
                            return 3.244423108548654;
                        } else {
                            return 3.776511060442418;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.3802870311041559) {
                            return 4.308283111055021;
                        } else {
                            return 5.556875906076276;
                        }
                    }
                }
            } else {
                if (p.stage0FullY88 <= 10.176928285126074) {
                    if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 15.135902575280472) {
                        if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.5804925164850561) {
                            return 5.344516076048126;
                        } else {
                            return 4.211248800974775;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.9088861977914864) {
                            return 6.097732248805999;
                        } else {
                            return 14.688311688311689;
                        }
                    }
                } else {
                    if ((p.stage0Y88Width * p.stage0Y88Depth) <= 47.072197533711886) {
                        if (p.stage0FullY104 <= 6.206560389234945) {
                            return 5.81360201511335;
                        } else {
                            return 8.525131282820706;
                        }
                    } else {
                        if (((double) p.stage0FullY96 / (p.stage0FullY88 + EPSILON)) <= 0.40491687533238613) {
                            return 10.130941064638783;
                        } else {
                            return 15.671732522796352;
                        }
                    }
                }
            }
        } else {
            if (p.stage0FullY96 <= 20.112653368205137) {
                if ((p.stage0Y88LargestCluster + p.stage0Y96LargestCluster) <= 4.557231319396399) {
                    if (p.stage0Y96LargestCluster <= 1.7135908313624517) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.45008614546979786) {
                            return 3.731676785821632;
                        } else {
                            return 6.919957081545064;
                        }
                    } else {
                        if (p.stage0Y88Depth <= 6.602429996525138) {
                            return 3.6978265744121983;
                        } else {
                            return 4.545901590645787;
                        }
                    }
                } else {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.46560481435033085) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.18395706111464133) {
                            return 4.001017681140829;
                        } else {
                            return 5.810484351310374;
                        }
                    } else {
                        if (scout <= 36.65340151621653) {
                            return 8.816777850615047;
                        } else {
                            return 12.828596037898363;
                        }
                    }
                }
            } else {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.5316966886283034) {
                    if (p.stage0Y96Width <= 11.026873282635059) {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 36.56119654260632) {
                            return 2.218870346598203;
                        } else {
                            return 7.288740371545084;
                        }
                    } else {
                        if (p.stage0FullY112 <= 14.228950703157555) {
                            return 8.783413881330464;
                        } else {
                            return 4.713986599664992;
                        }
                    }
                } else {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.7160665950573579) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.5746040559123003) {
                            return 12.32777115613826;
                        } else {
                            return 68.07407407407408;
                        }
                    } else {
                        return 76.95;
                    }
                }
            }
        }
    }

    private static double tree14(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        if (p.stage0FullY96 <= 11.399138444754826) {
            if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 16.925816585740918) {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.7215576793860226) {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.23380008214776077) {
                        if (p.stage0FullY88 <= 7.1701073149079955) {
                            return 2.6466901113317904;
                        } else {
                            return 4.635865150284322;
                        }
                    } else {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 7.184855190191817) {
                            return 1.8902102357188362;
                        } else {
                            return 3.703200058475258;
                        }
                    }
                } else {
                    if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 11.853989745887922) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.9988728267269196) {
                            return 4.125301204819277;
                        } else {
                            return 6.123864268496199;
                        }
                    } else {
                        if (p.stage0Y96LargestCluster <= 4.003559622915256) {
                            return 8.582515991471215;
                        } else {
                            return 5.492252066115703;
                        }
                    }
                }
            } else {
                if (p.stage0FullY96 <= 4.98791030374724) {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.1555993335847352) {
                        if (((double) p.stage0FullY104 / (p.stage0FullY96 + EPSILON)) <= 0.5673168646642067) {
                            return 2.9975030606964506;
                        } else {
                            return 3.32077262466653;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.6582853695724119) {
                            return 4.260495333150339;
                        } else {
                            return 7.972512276344753;
                        }
                    }
                } else {
                    if (p.stage0FullY96 <= 7.986768720199712) {
                        if (p.stage0FullY88 <= 8.761597258859126) {
                            return 4.096311343798752;
                        } else {
                            return 5.015526481715006;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.5061766538392805) {
                            return 5.189037181884629;
                        } else {
                            return 8.90829557180672;
                        }
                    }
                }
            }
        } else {
            if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.5584888044085501) {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.324168581363) {
                    if (p.stage0FullY96 <= 16.462229181296298) {
                        if (((double) p.stage0Y96LargestCluster / (p.stage0FullY96 + EPSILON)) <= 0.3642819099163745) {
                            return 6.252737020893446;
                        } else {
                            return 4.908891038606983;
                        }
                    } else {
                        if (scout <= 123.48220152709736) {
                            return 7.609797145678126;
                        } else {
                            return 5.726679841897234;
                        }
                    }
                } else {
                    if (((double) p.stage0Y96LargestCluster / (p.stage0FullY96 + EPSILON)) <= 0.7528295712036261) {
                        if ((p.stage0Y88Width * p.stage0Y88Depth) <= 159.54097590480976) {
                            return 7.721848687340961;
                        } else {
                            return 9.473589200219763;
                        }
                    } else {
                        if (((double) p.stage0FullY96 / (p.stage0FullY88 + EPSILON)) <= 0.8833293207028552) {
                            return 5.234375;
                        } else {
                            return 3.402527849185947;
                        }
                    }
                }
            } else {
                if (((double) p.stage0FullY104 / (p.stage0FullY96 + EPSILON)) <= 0.10233131377209088) {
                    return 59.325;
                } else {
                    if ((p.stage0Y88Width * p.stage0Y88Depth) <= 50.299711787281424) {
                        if (((double) p.stage0Y96LargestCluster / (p.stage0FullY96 + EPSILON)) <= 0.838961998560382) {
                            return 9.875326939843069;
                        } else {
                            return 5.394594594594595;
                        }
                    } else {
                        if ((p.stage0Y88LargestCluster + p.stage0Y96LargestCluster) <= 29.66362542084587) {
                            return 12.81190798376184;
                        } else {
                            return 81.23529411764706;
                        }
                    }
                }
            }
        }
    }

    private static double tree15(int scout, BetaTerrain173.PreparedStage0MonsterFeatures p) {
        if (p.stage0FullY88 <= 14.827591230081204) {
            if (p.stage0FullY88 <= 8.324902460130351) {
                if (p.stage0FullY88 <= 6.899691907176605) {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.6573522557090838) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.24031709346342126) {
                            return 3.2123402291470193;
                        } else {
                            return 4.065371318089742;
                        }
                    } else {
                        if ((p.stage0Y96Width * p.stage0Y96Depth) <= 6.719700160771469) {
                            return 4.959594603664676;
                        } else {
                            return 8.848826914563967;
                        }
                    }
                } else {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.5716315896373989) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.31758233969329547) {
                            return 3.952243633844927;
                        } else {
                            return 5.227178042598353;
                        }
                    } else {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 27.83778661901028) {
                            return 5.98679933011526;
                        } else {
                            return 8.747384155455904;
                        }
                    }
                }
            } else {
                if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.39783116584073114) {
                    if (p.stage0Y96Width <= 7.954688969978452) {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.2371390894701956) {
                            return 3.900290279603667;
                        } else {
                            return 5.140802933796564;
                        }
                    } else {
                        if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.1958516773601541) {
                            return 4.428781858765623;
                        } else {
                            return 5.83128248089679;
                        }
                    }
                } else {
                    if (p.stage0Y88LargestCluster <= 4.820693900725196) {
                        if (scout <= 12.710570499166181) {
                            return 59.56164383561644;
                        } else {
                            return 8.07370126113618;
                        }
                    } else {
                        if ((p.stage0FullY96 + p.stage0FullY104 + p.stage0FullY112) <= 21.12351474171668) {
                            return 6.321719505355092;
                        } else {
                            return 11.264812345208611;
                        }
                    }
                }
            }
        } else {
            if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.4769015119533638) {
                if (((double) p.stage0FullY96 / (p.stage0FullY88 + EPSILON)) <= 0.8906432459552974) {
                    if (((double) p.stage0FullY88 / (scout + EPSILON)) <= 0.3123185720175654) {
                        if ((p.stage0Y96Width * p.stage0Y96Depth) <= 133.2523313189169) {
                            return 6.2036581779685935;
                        } else {
                            return 5.425998630768586;
                        }
                    } else {
                        if ((p.stage0Y88Width + p.stage0Y88Depth + p.stage0Y96Width + p.stage0Y96Depth) <= 32.15025881032937) {
                            return 6.539367691641403;
                        } else {
                            return 7.953585283272333;
                        }
                    }
                } else {
                    if (scout <= 48.78481234786325) {
                        if (p.stage0FullY96 <= 18.899776796298) {
                            return 11.601990049751244;
                        } else {
                            return 4.429184549356223;
                        }
                    } else {
                        if (p.stage0Y88Depth <= 8.992954072073546) {
                            return 5.23782722513089;
                        } else {
                            return 7.078322416415733;
                        }
                    }
                }
            } else {
                if (p.stage0FullY104 <= 23.595483188329023) {
                    if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.5814592795263727) {
                        if (p.stage0Y96Width <= 11.187232648575321) {
                            return 13.287275064267352;
                        } else {
                            return 10.036369816451394;
                        }
                    } else {
                        if (((double) p.stage0Y88LargestCluster / (p.stage0FullY88 + EPSILON)) <= 0.6226519927088415) {
                            return 19.853700516351118;
                        } else {
                            return 6.654135338345864;
                        }
                    }
                } else {
                    if ((p.stage0Y88Width * p.stage0Y88Depth) <= 176.45893728009375) {
                        return 66.18181818181819;
                    } else {
                        return 98.1;
                    }
                }
            }
        }
    }

}
